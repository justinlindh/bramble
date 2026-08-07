/*
 * Behavior tests for the nRF message-store persistence backend
 * (components/msg_store/msg_store_lfs.c) against littlefs's RAM block
 * device. Unlike test_msg_store_spiffs.c, which can only assert that the
 * ESP backend's host stubs fail, this suite exercises the real code: the
 * derive-count-from-file-size recovery invariant, torn-trailing-record
 * truncation, corrupt-header recovery, the streaming rollover, and clear.
 *
 * The torn-file scenarios are staged by writing the store's file directly
 * through the shared lfs_t between backend sessions, exactly the state a
 * power cut mid-append leaves behind.
 */
#include "unity.h"

#include <string.h>

#include "lfs.h"
#include "lfs_nvmc.h"
#include "msg_store.h"
#include "msg_store_spiffs.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_lfs_mount.h"

#define MSG_FILE_PATH "/messages.bin"

/* Mirrors the backend's private header layout; asserted against real files
 * the backend itself wrote, so drift fails loudly here. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t record_count;
    uint32_t next_id;
} __attribute__((packed)) msg_file_header_t;

void setUp(void) {}
void tearDown(void) {}

/* Shaped like a record the store actually writes: a nonzero uid and a real
 * text_len, both of which the backend's identity check reads. */
static stored_msg_t make_msg(uint32_t id, const char* text) {
    stored_msg_t m;
    memset(&m, 0, sizeof(m));
    m.uid = id + 1;
    m.packet_id = id;
    strncpy(m.text, text, sizeof(m.text) - 1);
    m.text_len = (uint16_t)strlen(m.text);
    return m;
}

/* Rewrites the store's file to <full records> plus tail_bytes of garbage,
 * with the header's record_count field set to header_count. Used to stage
 * torn and lying-header files for init to recover from. */
static void stage_file(int full_records, int tail_bytes, uint32_t header_count) {
    lfs_t* fs = nvs_lfs_handle();
    TEST_ASSERT_NOT_NULL(fs);

    static uint8_t buffer[LFS_NVMC_CACHE_SIZE];
    static const struct lfs_file_config cfg = {.buffer = buffer};
    lfs_file_t f;
    TEST_ASSERT_EQUAL(LFS_ERR_OK, lfs_file_opencfg(fs, &f, MSG_FILE_PATH,
                                                   LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC, &cfg));

    msg_file_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = 0x4252414D;
    hdr.version = 1;
    hdr.record_size = sizeof(stored_msg_t);
    hdr.record_count = header_count;
    hdr.next_id = 100;
    TEST_ASSERT_EQUAL((lfs_ssize_t)sizeof(hdr), lfs_file_write(fs, &f, &hdr, sizeof(hdr)));

    for (int i = 0; i < full_records; i++) {
        stored_msg_t m = make_msg(1000u + (uint32_t)i, "staged record");
        TEST_ASSERT_EQUAL((lfs_ssize_t)sizeof(m), lfs_file_write(fs, &f, &m, sizeof(m)));
    }
    for (int i = 0; i < tail_bytes; i++) {
        uint8_t junk = 0xA5;
        TEST_ASSERT_EQUAL(1, lfs_file_write(fs, &f, &junk, 1));
    }
    TEST_ASSERT_EQUAL(LFS_ERR_OK, lfs_file_close(fs, &f));
}

void test_init_creates_empty_store(void) {
    msg_store_spiffs_clear();
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_get_count());
}

void test_save_and_load_roundtrip(void) {
    msg_store_spiffs_clear();
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());

    for (uint32_t i = 0; i < 5; i++) {
        char text[32];
        snprintf(text, sizeof(text), "message %lu", (unsigned long)i);
        stored_msg_t m = make_msg(i, text);
        TEST_ASSERT_EQUAL(0, msg_store_spiffs_save(&m));
    }
    TEST_ASSERT_EQUAL(5, msg_store_spiffs_get_count());

    stored_msg_t out[3];
    /* load_recent returns the LAST max_count records in file order. */
    TEST_ASSERT_EQUAL(3, msg_store_spiffs_load_recent(out, 3));
    TEST_ASSERT_EQUAL_UINT32(2, out[0].packet_id);
    TEST_ASSERT_EQUAL_UINT32(4, out[2].packet_id);
    TEST_ASSERT_EQUAL_STRING("message 4", out[2].text);
}

void test_recovery_trusts_file_size_over_header(void) {
    /* A crash between record append and header update leaves the header
     * counting fewer records than the file holds. Every fully written
     * record must survive. */
    msg_store_spiffs_clear();
    stage_file(3, 0, /*header_count=*/1);
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    TEST_ASSERT_EQUAL(3, msg_store_spiffs_get_count());

    stored_msg_t out[4];
    TEST_ASSERT_EQUAL(3, msg_store_spiffs_load_recent(out, 4));
    TEST_ASSERT_EQUAL_UINT32(1002, out[2].packet_id);
}

void test_recovery_truncates_torn_trailing_record(void) {
    /* A crash mid-append leaves a partial record; it must be dropped and
     * the next append must land aligned. */
    msg_store_spiffs_clear();
    stage_file(2, 100, /*header_count=*/2);
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    TEST_ASSERT_EQUAL(2, msg_store_spiffs_get_count());

    stored_msg_t m = make_msg(7777, "post-recovery append");
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_save(&m));
    TEST_ASSERT_EQUAL(3, msg_store_spiffs_get_count());

    stored_msg_t out[3];
    TEST_ASSERT_EQUAL(3, msg_store_spiffs_load_recent(out, 3));
    TEST_ASSERT_EQUAL_UINT32(7777, out[2].packet_id);
    TEST_ASSERT_EQUAL_STRING("post-recovery append", out[2].text);
}

void test_corrupt_header_recreates_store(void) {
    msg_store_spiffs_clear();
    lfs_t* fs = nvs_lfs_handle();
    static uint8_t buffer[LFS_NVMC_CACHE_SIZE];
    static const struct lfs_file_config cfg = {.buffer = buffer};
    lfs_file_t f;
    TEST_ASSERT_EQUAL(LFS_ERR_OK, lfs_file_opencfg(fs, &f, MSG_FILE_PATH,
                                                   LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC, &cfg));
    const char junk[] = "not a message store";
    TEST_ASSERT_EQUAL((lfs_ssize_t)sizeof(junk), lfs_file_write(fs, &f, junk, sizeof(junk)));
    TEST_ASSERT_EQUAL(LFS_ERR_OK, lfs_file_close(fs, &f));

    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_get_count());
}

void test_rollover_keeps_most_recent_records(void) {
    msg_store_spiffs_clear();
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    for (uint32_t i = 0; i < 12; i++) {
        stored_msg_t m = make_msg(i, "rollover fill");
        TEST_ASSERT_EQUAL(0, msg_store_spiffs_save(&m));
    }
    TEST_ASSERT_EQUAL(12, msg_store_spiffs_get_count());

    /* 12 > 10, keep 75% of 10 = 7, so ids 5..11 survive. */
    msg_store_spiffs_rollover(10, 75);
    TEST_ASSERT_EQUAL(7, msg_store_spiffs_get_count());

    stored_msg_t out[7];
    TEST_ASSERT_EQUAL(7, msg_store_spiffs_load_recent(out, 7));
    TEST_ASSERT_EQUAL_UINT32(5, out[0].packet_id);
    TEST_ASSERT_EQUAL_UINT32(11, out[6].packet_id);

    /* The store must still accept appends after the swap. */
    stored_msg_t m = make_msg(99, "post-rollover");
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_save(&m));
    TEST_ASSERT_EQUAL(8, msg_store_spiffs_get_count());
}

/* A delivery status arrives after the record was appended, so the backend has
 * to rewrite that record where it lies. from_end counts back from the newest,
 * and the record on the filesystem must carry the new status afterwards. */
void test_update_rewrites_a_record_in_place(void) {
    msg_store_spiffs_clear();
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    for (uint32_t i = 0; i < 3; i++) {
        char text[32];
        snprintf(text, sizeof(text), "sent %lu", (unsigned long)i);
        stored_msg_t m = make_msg(i, text);
        m.status = MSG_STATUS_SENT;
        TEST_ASSERT_EQUAL(0, msg_store_spiffs_save(&m));
    }

    /* The middle record: from_end 1 of 3. */
    stored_msg_t updated = make_msg(1, "sent 1");
    updated.status = MSG_STATUS_DELIVERED;
    updated.route_hop_count = 2;
    updated.route_hops[0] = 0xAAAA;
    updated.route_hops[1] = 0xBBBB;
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_update(1, &updated));

    stored_msg_t out[3];
    TEST_ASSERT_EQUAL(3, msg_store_spiffs_load_recent(out, 3));
    TEST_ASSERT_EQUAL(MSG_STATUS_SENT, out[0].status);
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, out[1].status);
    TEST_ASSERT_EQUAL(2, out[1].route_hop_count);
    TEST_ASSERT_EQUAL_UINT32(0xBBBB, out[1].route_hops[1]);
    TEST_ASSERT_EQUAL_STRING("sent 1", out[1].text);
    TEST_ASSERT_EQUAL(MSG_STATUS_SENT, out[2].status);
}

/* The record count never moves, so a reopened store still sees exactly the
 * records it had, with the updated status among them. */
void test_updated_status_survives_a_reopen(void) {
    msg_store_spiffs_clear();
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    stored_msg_t m = make_msg(42, "awaiting ack");
    m.status = MSG_STATUS_SENT;
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_save(&m));

    m.status = MSG_STATUS_DELIVERED;
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_update(0, &m));

    stored_msg_t out[1];
    TEST_ASSERT_EQUAL(1, msg_store_spiffs_get_count());
    TEST_ASSERT_EQUAL(1, msg_store_spiffs_load_recent(out, 1));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, out[0].status);
}

/* If the caller's idea of which record it means has drifted, the update must
 * be dropped: stamping a status onto another message would be worse than
 * losing it. */
void test_update_rejects_a_different_message(void) {
    msg_store_spiffs_clear();
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    stored_msg_t a = make_msg(1, "mine");
    a.status = MSG_STATUS_SENT;
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_save(&a));

    stored_msg_t other = make_msg(2, "someone else's");
    other.status = MSG_STATUS_DELIVERED;
    TEST_ASSERT_EQUAL(-1, msg_store_spiffs_update(0, &other));

    stored_msg_t out[1];
    TEST_ASSERT_EQUAL(1, msg_store_spiffs_load_recent(out, 1));
    TEST_ASSERT_EQUAL(MSG_STATUS_SENT, out[0].status);
    TEST_ASSERT_EQUAL_STRING("mine", out[0].text);
}

void test_update_rejects_positions_outside_the_file(void) {
    msg_store_spiffs_clear();
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    stored_msg_t m = make_msg(1, "only one");
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_save(&m));

    TEST_ASSERT_EQUAL(-1, msg_store_spiffs_update(-1, &m));
    TEST_ASSERT_EQUAL(-1, msg_store_spiffs_update(1, &m));
    TEST_ASSERT_EQUAL(-1, msg_store_spiffs_update(0, NULL));
}

/* A restored row's timestamp is zeroed in RAM (it is a previous boot's uptime
 * clock), so writing that row back must not erase the one on the filesystem. */
void test_update_keeps_the_persisted_timestamp(void) {
    msg_store_spiffs_clear();
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    stored_msg_t m = make_msg(5, "timed");
    m.status = MSG_STATUS_SENT;
    m.timestamp_s = 4242;
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_save(&m));

    stored_msg_t restored = m;
    restored.timestamp_s = 0;
    restored.status = MSG_STATUS_DELIVERED;
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_update(0, &restored));

    stored_msg_t out[1];
    TEST_ASSERT_EQUAL(1, msg_store_spiffs_load_recent(out, 1));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, out[0].status);
    TEST_ASSERT_EQUAL_UINT32(4242, out[0].timestamp_s);
}

void test_rollover_below_threshold_is_a_no_op(void) {
    msg_store_spiffs_clear();
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    for (uint32_t i = 0; i < 3; i++) {
        stored_msg_t m = make_msg(i, "few");
        TEST_ASSERT_EQUAL(0, msg_store_spiffs_save(&m));
    }
    msg_store_spiffs_rollover(10, 75);
    TEST_ASSERT_EQUAL(3, msg_store_spiffs_get_count());
}

void test_clear_empties_the_store(void) {
    msg_store_spiffs_clear();
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    stored_msg_t m = make_msg(1, "doomed");
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_save(&m));
    msg_store_spiffs_clear();
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_get_count());
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_init());
    TEST_ASSERT_EQUAL(0, msg_store_spiffs_get_count());
}

int main(void) {
    /* Mount the RAM-backed filesystem once; nvs_flash_init formats it. */
    TEST_ASSERT_EQUAL(0, nvs_flash_init());

    UNITY_BEGIN();
    RUN_TEST(test_init_creates_empty_store);
    RUN_TEST(test_save_and_load_roundtrip);
    RUN_TEST(test_recovery_trusts_file_size_over_header);
    RUN_TEST(test_recovery_truncates_torn_trailing_record);
    RUN_TEST(test_corrupt_header_recreates_store);
    RUN_TEST(test_rollover_keeps_most_recent_records);
    RUN_TEST(test_rollover_below_threshold_is_a_no_op);
    RUN_TEST(test_update_rewrites_a_record_in_place);
    RUN_TEST(test_updated_status_survives_a_reopen);
    RUN_TEST(test_update_rejects_a_different_message);
    RUN_TEST(test_update_rejects_positions_outside_the_file);
    RUN_TEST(test_update_keeps_the_persisted_timestamp);
    RUN_TEST(test_clear_empties_the_store);
    return UNITY_END();
}
