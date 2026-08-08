/*
 * Delivery status survives a reboot: the RAM store's side of it.
 *
 * The status of an outgoing message changes long after the row was appended
 * to flash (an ACK or a broadcast delivery receipt arrives seconds later), so
 * msg_store has to push that change back into the already-written record. It
 * addresses the record by distance from the newest, relying on the ring and
 * the file being suffixes of the same append sequence. That invariant is what
 * this suite pins down, through ring wrap, through rollover, and through an
 * append that failed and skewed the mapping.
 *
 * The backend here is a reference implementation of msg_store_spiffs.h over a
 * plain array: same contract as the SPIFFS and littlefs backends (which are
 * ESP-only and nRF-only), so the store's wiring can be exercised on the host.
 * The real flash behavior is covered by test_msg_store_lfs.c.
 */
#include "unity.h"

#include "msg_store.h"
#include "msg_store_spiffs.h"

#include <stdio.h>
#include <string.h>

#define FAKE_CAPACITY 32

static stored_msg_t s_records[FAKE_CAPACITY];
static int s_record_count;
static bool s_fail_next_save;
static int s_update_calls;
static int s_update_rejects;

static void fake_reset(void) {
    memset(s_records, 0, sizeof(s_records));
    s_record_count = 0;
    s_fail_next_save = false;
    s_update_calls = 0;
    s_update_rejects = 0;
}

int msg_store_spiffs_init(void) { return 0; }

int msg_store_spiffs_save(const stored_msg_t* msg) {
    if (s_fail_next_save) {
        s_fail_next_save = false;
        return -1;
    }
    if (!msg || s_record_count >= FAKE_CAPACITY)
        return -1;
    s_records[s_record_count++] = *msg;
    return 0;
}

int msg_store_spiffs_get_count(void) { return s_record_count; }

int msg_store_spiffs_update(int from_end, const stored_msg_t* msg) {
    s_update_calls++;
    if (!msg || from_end < 0 || from_end >= s_record_count) {
        s_update_rejects++;
        return -1;
    }
    int index = s_record_count - 1 - from_end;
    if (!msg_store_record_matches(&s_records[index], msg)) {
        s_update_rejects++;
        return -1;
    }
    uint32_t persisted_ts = s_records[index].timestamp_s;
    s_records[index] = *msg;
    s_records[index].timestamp_s = persisted_ts;
    return 0;
}

int msg_store_spiffs_load_recent(stored_msg_t* msgs, int max_count) {
    if (!msgs || max_count <= 0)
        return 0;
    int to_load = (s_record_count < max_count) ? s_record_count : max_count;
    memcpy(msgs, &s_records[s_record_count - to_load], (size_t)to_load * sizeof(stored_msg_t));
    return to_load;
}

void msg_store_spiffs_rollover(int max_messages, int keep_pct) {
    if (s_record_count <= max_messages)
        return;
    int keep = (max_messages * keep_pct) / 100;
    memmove(s_records, &s_records[s_record_count - keep], (size_t)keep * sizeof(stored_msg_t));
    s_record_count = keep;
}

void msg_store_spiffs_clear(void) { fake_reset(); }

void setUp(void) {
    fake_reset();
    msg_store_init();
}
void tearDown(void) {}

/* Sends one outgoing DM the way the pipeline does: store the row, then stamp
 * the wire packet id onto it. Returns the packet id. */
static uint32_t send_dm(uint32_t peer, const char* text, uint32_t packet_id) {
    uint32_t uid = msg_store_next_uid();
    msg_store_add_dm_uid(peer, MSG_DIR_OUTGOING, text, strlen(text), 0, 0, 0, MSG_STATUS_NONE, uid);
    TEST_ASSERT_TRUE(msg_store_update_by_uid(uid, packet_id, MSG_STATUS_SENT));
    return packet_id;
}

static const stored_msg_t* record_for_text(const char* text) {
    for (int i = 0; i < s_record_count; i++) {
        if (strcmp(s_records[i].text, text) == 0)
            return &s_records[i];
    }
    return NULL;
}

void test_delivered_status_reaches_the_persisted_record(void) {
    send_dm(0x1111, "hello", 0xAA);
    const stored_msg_t* rec = record_for_text("hello");
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL(MSG_STATUS_SENT, rec->status);

    uint32_t route[] = {0x1000, 0x2000, 0x1111};
    TEST_ASSERT_TRUE(msg_store_update_status_with_route(0xAA, MSG_STATUS_DELIVERED, 3, route));

    rec = record_for_text("hello");
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, rec->status);
    TEST_ASSERT_EQUAL(3, rec->route_hop_count);
    TEST_ASSERT_EQUAL_UINT32(0x2000, rec->route_hops[1]);
}

void test_failed_status_reaches_the_persisted_record(void) {
    uint32_t uid = msg_store_next_uid();
    msg_store_add_dm_uid(0x2222, MSG_DIR_OUTGOING, "nope", 4, 0, 0, 0, MSG_STATUS_NONE, uid);
    TEST_ASSERT_TRUE(msg_store_update_by_uid(uid, 0, MSG_STATUS_FAILED));

    const stored_msg_t* rec = record_for_text("nope");
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL(MSG_STATUS_FAILED, rec->status);
}

/* The ring is smaller than the file, so once it wraps the row being updated
 * is no longer at the same offset in both. Distance from the newest is what
 * still lines up, and this is the case that proves it. */
void test_update_lands_correctly_after_the_ring_wraps(void) {
    char text[8];
    for (int i = 0; i < MSG_STORE_MAX + 2; i++) {
        snprintf(text, sizeof(text), "m%d", i);
        send_dm(0x3333, text, 0x100u + (uint32_t)i);
    }
    TEST_ASSERT_EQUAL(MSG_STORE_MAX, msg_store_count());
    TEST_ASSERT_EQUAL(MSG_STORE_MAX + 2, s_record_count);

    /* Oldest row still in the ring, and the newest one. */
    int oldest = 2;
    int newest = MSG_STORE_MAX + 1;
    TEST_ASSERT_TRUE(msg_store_update_status_with_route(0x100u + (uint32_t)oldest,
                                                        MSG_STATUS_DELIVERED, 0, NULL));
    TEST_ASSERT_TRUE(
        msg_store_update_status_with_route(0x100u + (uint32_t)newest, MSG_STATUS_FAILED, 0, NULL));
    TEST_ASSERT_EQUAL(0, s_update_rejects);

    snprintf(text, sizeof(text), "m%d", oldest);
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, record_for_text(text)->status);
    snprintf(text, sizeof(text), "m%d", newest);
    TEST_ASSERT_EQUAL(MSG_STATUS_FAILED, record_for_text(text)->status);

    /* Everything else keeps the status it was stored with. */
    for (int i = 0; i < MSG_STORE_MAX + 2; i++) {
        if (i == oldest || i == newest)
            continue;
        snprintf(text, sizeof(text), "m%d", i);
        TEST_ASSERT_EQUAL(MSG_STATUS_SENT, record_for_text(text)->status);
    }
}

/* Rollover drops records off the front of the file. It keeps more records
 * than the ring holds, so distance from the newest is unaffected. */
void test_update_lands_correctly_after_rollover(void) {
    char text[8];
    for (int i = 0; i < 10; i++) {
        snprintf(text, sizeof(text), "r%d", i);
        send_dm(0x4444, text, 0x200u + (uint32_t)i);
    }
    /* The store rolled the file over; the ring is untouched. */
    TEST_ASSERT_TRUE(s_record_count < 10);
    TEST_ASSERT_TRUE(s_record_count >= msg_store_count());

    TEST_ASSERT_TRUE(msg_store_update_status_with_route(0x209, MSG_STATUS_DELIVERED, 0, NULL));
    TEST_ASSERT_EQUAL(0, s_update_rejects);
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, record_for_text("r9")->status);
    TEST_ASSERT_EQUAL(MSG_STATUS_SENT, record_for_text("r8")->status);
}

/* Repeat delivery receipts for one broadcast all say DELIVERED. Only the
 * first changes anything, and only the first is allowed to write: a broadcast
 * confirmed by twenty peers must not rewrite the record twenty times. */
void test_repeat_receipts_write_once(void) {
    msg_store_add_channel(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT, "all", 3, 0, 0, 0x55, MSG_STATUS_SENT,
                          0);
    uint32_t route_a[] = {0x1, 0x2};
    uint32_t route_b[] = {0x1, 0x3};

    TEST_ASSERT_TRUE(msg_store_update_status_with_route(0x55, MSG_STATUS_DELIVERED, 2, route_a));
    TEST_ASSERT_EQUAL(1, s_update_calls);

    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(
            msg_store_update_status_with_route(0x55, MSG_STATUS_DELIVERED, 2, route_b));
    }
    TEST_ASSERT_EQUAL(1, s_update_calls);
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, record_for_text("all")->status);
}

/* An append that failed leaves the ring one row ahead of the file, so
 * distance from the newest points at the wrong record. The backend's identity
 * check has to catch that: the wrong record must keep its own status. */
void test_skewed_mapping_is_rejected_not_corrupted(void) {
    send_dm(0x5555, "kept", 0x11);
    s_fail_next_save = true;
    send_dm(0x5555, "lost", 0x22);
    TEST_ASSERT_EQUAL(1, s_record_count);
    int rejects_before = s_update_rejects;

    /* "lost" is the newest ring row but has no record; from_end 0 resolves to
     * the record for "kept". */
    TEST_ASSERT_TRUE(msg_store_update_status_with_route(0x22, MSG_STATUS_DELIVERED, 0, NULL));
    TEST_ASSERT_EQUAL(rejects_before + 1, s_update_rejects);
    TEST_ASSERT_EQUAL(MSG_STATUS_SENT, record_for_text("kept")->status);
}

/* The whole point: reload the store the way a reboot does and the delivery
 * status is still there. */
void test_status_survives_a_reload(void) {
    send_dm(0x6666, "one", 0x31);
    send_dm(0x6666, "two", 0x32);
    TEST_ASSERT_TRUE(msg_store_update_status_with_route(0x31, MSG_STATUS_DELIVERED, 0, NULL));

    msg_store_init_with_persistence();

    TEST_ASSERT_EQUAL(2, msg_store_count());
    const stored_msg_t* first = msg_store_get(0);
    const stored_msg_t* second = msg_store_get(1);
    TEST_ASSERT_EQUAL_STRING("one", first->text);
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, first->status);
    TEST_ASSERT_EQUAL_STRING("two", second->text);
    TEST_ASSERT_EQUAL(MSG_STATUS_SENT, second->status);
}

/* Restored rows carry uids from the previous boot. The allocator has to
 * resume past them, or a fresh message gets a uid a live row already holds
 * and the two become indistinguishable to update-by-uid. */
void test_uid_allocator_resumes_past_restored_rows(void) {
    send_dm(0x7777, "a", 0x41);
    send_dm(0x7777, "b", 0x42);
    uint32_t highest = msg_store_get(1)->uid;

    msg_store_init_with_persistence();

    TEST_ASSERT_TRUE(msg_store_next_uid() > highest);
}

/* The entire persistence argument for "send when online" is that parking
 * is just a status change and msg_store already pushes those to flash.
 * If this fails, a parked message silently evaporates on reboot, which is
 * exactly the class of loss the feature exists to remove. */
void test_parked_status_reaches_the_persisted_record(void) {
    msg_store_add_dm_uid(0xAABBCCDD, MSG_DIR_OUTGOING, "parked", 6, 0, 0, 0, MSG_STATUS_FAILED, 42);
    TEST_ASSERT_TRUE(msg_store_update_by_uid(42, 0, MSG_STATUS_QUEUED));

    /* The newest persisted record is the one that was just rewritten. */
    TEST_ASSERT_EQUAL(MSG_STATUS_QUEUED, s_records[s_record_count - 1].status);
    TEST_ASSERT_EQUAL_UINT32(42, s_records[s_record_count - 1].uid);
    TEST_ASSERT_EQUAL_STRING("parked", s_records[s_record_count - 1].text);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_delivered_status_reaches_the_persisted_record);
    RUN_TEST(test_failed_status_reaches_the_persisted_record);
    RUN_TEST(test_update_lands_correctly_after_the_ring_wraps);
    RUN_TEST(test_update_lands_correctly_after_rollover);
    RUN_TEST(test_repeat_receipts_write_once);
    RUN_TEST(test_skewed_mapping_is_rejected_not_corrupted);
    RUN_TEST(test_status_survives_a_reload);
    RUN_TEST(test_uid_allocator_resumes_past_restored_rows);
    RUN_TEST(test_parked_status_reaches_the_persisted_record);
    return UNITY_END();
}
