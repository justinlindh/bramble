#include "unity.h"
#include "msg_store.h"
#include <string.h>

/*
 * Invariant under test: ONE user-submitted message == exactly ONE msg_store
 * row, whose status advances NONE (pending) -> SENT -> DELIVERED/FAILED.
 *
 * The bug this pins: a message had no stable identity across the send
 * pipeline, so every stage called msg_store_add_* instead of updating the row
 * an earlier stage had already created. A DM to a multi-hop peer with a cold
 * DM session produced THREE rows (mesh_send_message's awaiting-route pending
 * row, mesh_send_dm's awaiting-session pending row after the route-discovery
 * flush re-entered the send path, and flush_session_queue's real SENT row).
 * Only the last one ever went DELIVERED, so two pending duplicates lingered
 * forever in the chat UI.
 *
 * main/mesh_task.c is ESP-IDF-only and never host-compiled (same constraint
 * test_flood_origination.c works under), so the pipeline below MIRRORS its
 * stages. It makes exactly the msg_store calls the real stages make, in the
 * same order, and threads the same uid: it is the msg_store contract that is
 * being pinned, and it is that contract (uid allocated once at the entry
 * point, updated in place downstream) that the fix relies on.
 */

#define PEER 0xAABBCCDDu
#define CHANNEL_IDX 0

/* ---- Mirror of the mesh_task send pipeline ------------------------------ */

#define H_MAX_QUEUED 8
#define H_REASON_ROUTE 0
#define H_REASON_SESSION 1

typedef struct {
    bool used;
    uint8_t reason;
    uint32_t dest_addr;
    char text[64];
    size_t len;
    uint32_t uid;
    int16_t channel_idx;
} h_queued_msg_t;

static h_queued_msg_t h_queue[H_MAX_QUEUED];
static bool h_have_route;
static bool h_have_session;
static bool h_tx_ok;
static uint32_t h_next_pkt_id;
static int h_tx_count; /* how many frames actually hit the "radio" */

static void h_reset(void) {
    memset(h_queue, 0, sizeof(h_queue));
    h_have_route = false;
    h_have_session = false;
    h_tx_ok = true;
    h_next_pkt_id = 0x1000;
    h_tx_count = 0;
    msg_store_init();
}

/* Mirrors send_dm_packet: mints the real wire packet_id. */
static uint32_t h_send_dm_packet(void) {
    if (!h_tx_ok)
        return 0;
    h_tx_count++;
    return h_next_pkt_id++;
}

static int h_queue_push(uint8_t reason, uint32_t dest, const char* text, size_t len, uint32_t uid,
                        int channel_idx) {
    for (int i = 0; i < H_MAX_QUEUED; i++) {
        if (h_queue[i].used)
            continue;
        h_queue[i].used = true;
        h_queue[i].reason = reason;
        h_queue[i].dest_addr = dest;
        memcpy(h_queue[i].text, text, len);
        h_queue[i].len = len;
        h_queue[i].uid = uid;
        h_queue[i].channel_idx = (int16_t)channel_idx;
        return 0;
    }
    return -3;
}

/* Mirrors mesh_send_dm(channel_idx, dest, data, len, uid). */
static uint32_t h_send_dm(uint32_t dest, const char* text, size_t len, uint32_t uid) {
    if (h_have_session) {
        uint32_t pkt_id = h_send_dm_packet();
        if (pkt_id != 0) {
            if (!msg_store_update_by_uid(uid, pkt_id, MSG_STATUS_SENT)) {
                msg_store_add_dm_uid(dest, MSG_DIR_OUTGOING, text, len, 0, 0, pkt_id,
                                     MSG_STATUS_SENT, uid ? uid : msg_store_next_uid());
            }
        } else {
            msg_store_update_by_uid(uid, 0, MSG_STATUS_FAILED);
        }
        return pkt_id;
    }

    /* No session: queue and (in the real code) fire the KE INIT. */
    uint32_t row_uid = (uid != 0) ? uid : msg_store_next_uid();
    if (h_queue_push(H_REASON_SESSION, dest, text, len, row_uid, CHANNEL_IDX) != 0) {
        msg_store_update_by_uid(uid, 0, MSG_STATUS_FAILED);
        return 0;
    }
    if (uid == 0) {
        msg_store_add_dm_uid(dest, MSG_DIR_OUTGOING, text, len, 0, 0, 0, MSG_STATUS_NONE, row_uid);
    }
    return h_next_pkt_id++; /* caller-facing tracking placeholder, never on the wire */
}

/* Mirrors mesh_send_message_uid: route check, then the channel/DM path. */
static uint32_t h_send_message(uint32_t dest, const char* text, size_t len, uint32_t uid) {
    if (!h_have_route) {
        uint32_t row_uid = (uid != 0) ? uid : msg_store_next_uid();
        if (h_queue_push(H_REASON_ROUTE, dest, text, len, row_uid, CHANNEL_IDX) != 0) {
            msg_store_update_by_uid(uid, 0, MSG_STATUS_FAILED);
            return 0;
        }
        if (uid == 0) {
            msg_store_add_dm_uid(dest, MSG_DIR_OUTGOING, text, len, 0, 0, 0, MSG_STATUS_NONE,
                                 row_uid);
        }
        return 1; /* queued: nonzero, but no packet_id yet */
    }
    return h_send_dm(dest, text, len, uid);
}

/* Mirrors flush_queued_messages: route discovery completed. */
static void h_route_established(uint32_t dest) {
    for (int i = 0; i < H_MAX_QUEUED; i++) {
        if (!h_queue[i].used || h_queue[i].reason != H_REASON_ROUTE ||
            h_queue[i].dest_addr != dest) {
            continue;
        }
        h_send_message(dest, h_queue[i].text, h_queue[i].len, h_queue[i].uid);
        h_queue[i].used = false;
    }
}

/* Mirrors flush_session_queue: the KE handshake completed. */
static void h_session_established(uint32_t dest) {
    for (int i = 0; i < H_MAX_QUEUED; i++) {
        if (!h_queue[i].used || h_queue[i].reason != H_REASON_SESSION ||
            h_queue[i].dest_addr != dest) {
            continue;
        }
        uint32_t pkt_id = h_have_session ? h_send_dm_packet() : 0;
        if (pkt_id != 0) {
            if (!msg_store_update_by_uid(h_queue[i].uid, pkt_id, MSG_STATUS_SENT)) {
                msg_store_add_dm_uid(dest, MSG_DIR_OUTGOING, h_queue[i].text, h_queue[i].len, 0, 0,
                                     pkt_id, MSG_STATUS_SENT, h_queue[i].uid);
            }
        } else {
            msg_store_update_by_uid(h_queue[i].uid, 0, MSG_STATUS_FAILED);
        }
        h_queue[i].used = false;
    }
}

/* Mirrors the queue reaper's TTL expiry. */
static void h_expire_queue(void) {
    for (int i = 0; i < H_MAX_QUEUED; i++) {
        if (!h_queue[i].used)
            continue;
        msg_store_update_by_uid(h_queue[i].uid, 0, MSG_STATUS_FAILED);
        h_queue[i].used = false;
    }
}

/* ---- msg_store uid API -------------------------------------------------- */

void test_uid_alloc_is_monotonic_and_never_zero(void) {
    msg_store_init();
    uint32_t a = msg_store_next_uid();
    uint32_t b = msg_store_next_uid();
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_NOT_EQUAL(0, b);
    TEST_ASSERT_NOT_EQUAL(a, b);
}

void test_update_by_uid_stamps_packet_id_and_status(void) {
    msg_store_init();
    uint32_t uid = msg_store_next_uid();
    msg_store_add_dm_uid(PEER, MSG_DIR_OUTGOING, "hi", 2, 0, 0, 0, MSG_STATUS_NONE, uid);

    TEST_ASSERT_TRUE(msg_store_update_by_uid(uid, 0x2222, MSG_STATUS_SENT));
    TEST_ASSERT_EQUAL_INT(1, msg_store_count());
    TEST_ASSERT_EQUAL_UINT32(0x2222, msg_store_get(0)->packet_id);
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_SENT, msg_store_get(0)->status);

    /* packet_id 0 means "status only": the stamped id must survive. */
    TEST_ASSERT_TRUE(msg_store_update_by_uid(uid, 0, MSG_STATUS_DELIVERED));
    TEST_ASSERT_EQUAL_UINT32(0x2222, msg_store_get(0)->packet_id);
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_DELIVERED, msg_store_get(0)->status);
}

void test_update_by_uid_zero_never_matches(void) {
    msg_store_init();
    msg_store_add_ex2(PEER, MSG_DIR_OUTGOING, "hi", 2, 0, 0, 7, MSG_STATUS_SENT, 0);
    TEST_ASSERT_EQUAL_UINT32(0, msg_store_get(0)->uid); /* legacy adds stay untracked */
    TEST_ASSERT_FALSE(msg_store_update_by_uid(0, 9, MSG_STATUS_FAILED));
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_SENT, msg_store_get(0)->status);
    TEST_ASSERT_FALSE(msg_store_update_by_uid(12345, 9, MSG_STATUS_FAILED)); /* unknown uid */
}

/* ---- The pipeline invariant --------------------------------------------- */

/*
 * The regression: cold peer, no route AND no DM session. Pre-fix this stored
 * three rows (pending, pending, sent). It must store exactly one.
 */
void test_cold_peer_send_stores_exactly_one_row(void) {
    h_reset();
    const char* text = "hello";
    size_t len = strlen(text);

    /* Stage 1: no route. Queued, one pending row. */
    TEST_ASSERT_NOT_EQUAL(0, h_send_message(PEER, text, len, 0));
    TEST_ASSERT_EQUAL_INT(1, msg_store_count());
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_NONE, msg_store_get(0)->status);
    uint32_t uid = msg_store_get(0)->uid;
    TEST_ASSERT_NOT_EQUAL(0, uid);

    /* Stage 2: route discovery completes, message re-enters the send path and
     * lands in the session queue (still no DM session). STILL one row. */
    h_have_route = true;
    h_route_established(PEER);
    TEST_ASSERT_EQUAL_INT(1, msg_store_count());
    TEST_ASSERT_EQUAL_UINT32(uid, msg_store_get(0)->uid);
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_NONE, msg_store_get(0)->status);
    TEST_ASSERT_EQUAL_UINT32(0, msg_store_get(0)->packet_id);

    /* Stage 3: KE handshake completes, the queued DM is transmitted. STILL one
     * row, now SENT and carrying the real wire packet_id. */
    h_have_session = true;
    h_session_established(PEER);
    TEST_ASSERT_EQUAL_INT(1, msg_store_count());
    TEST_ASSERT_EQUAL_INT(1, h_tx_count);
    const stored_msg_t* m = msg_store_get(0);
    TEST_ASSERT_EQUAL_UINT32(uid, m->uid);
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_SENT, m->status);
    TEST_ASSERT_NOT_EQUAL(0, m->packet_id);
    TEST_ASSERT_EQUAL_STRING(text, m->text);
    /* A DM stays channel-less through every stage (bug F1's convention): the
     * reconciled row must not drift into a channel thread. */
    TEST_ASSERT_EQUAL_INT(MSG_STORE_DM_CHANNEL, m->channel_index);
    uint32_t wire_pkt_id = m->packet_id;

    /* Stage 4: the ACK still correlates by packet_id, and lands on that row. */
    TEST_ASSERT_TRUE(msg_store_update_status(wire_pkt_id, MSG_STATUS_DELIVERED));
    TEST_ASSERT_EQUAL_INT(1, msg_store_count());
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_DELIVERED, msg_store_get(0)->status);
}

/* Route known, session cold: two stages, still one row. */
void test_cold_session_send_stores_exactly_one_row(void) {
    h_reset();
    h_have_route = true;

    TEST_ASSERT_NOT_EQUAL(0, h_send_message(PEER, "yo", 2, 0));
    TEST_ASSERT_EQUAL_INT(1, msg_store_count());
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_NONE, msg_store_get(0)->status);
    TEST_ASSERT_EQUAL_INT(0, h_tx_count);

    h_have_session = true;
    h_session_established(PEER);
    TEST_ASSERT_EQUAL_INT(1, msg_store_count());
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_SENT, msg_store_get(0)->status);
    TEST_ASSERT_NOT_EQUAL(0, msg_store_get(0)->packet_id);
}

/* Happy path (route known, session ACTIVE) must not regress: one row, SENT. */
void test_happy_path_stores_exactly_one_row(void) {
    h_reset();
    h_have_route = true;
    h_have_session = true;

    uint32_t pkt_id = h_send_message(PEER, "sup", 3, 0);
    TEST_ASSERT_NOT_EQUAL(0, pkt_id);
    TEST_ASSERT_EQUAL_INT(1, msg_store_count());
    const stored_msg_t* m = msg_store_get(0);
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_SENT, m->status);
    TEST_ASSERT_EQUAL_UINT32(pkt_id, m->packet_id);
    TEST_ASSERT_NOT_EQUAL(0, m->uid);

    TEST_ASSERT_TRUE(msg_store_update_status(pkt_id, MSG_STATUS_DELIVERED));
    TEST_ASSERT_EQUAL_INT(1, msg_store_count());
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_DELIVERED, msg_store_get(0)->status);
}

/* A message that never gets a session is retired as FAILED, not left pending. */
void test_expired_queue_entry_fails_its_one_row(void) {
    h_reset();
    h_have_route = true;

    h_send_message(PEER, "lost", 4, 0);
    TEST_ASSERT_EQUAL_INT(1, msg_store_count());

    h_expire_queue();
    TEST_ASSERT_EQUAL_INT(1, msg_store_count());
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_FAILED, msg_store_get(0)->status);
}

/* Two distinct sends stay two distinct rows (the fix must not collapse them). */
void test_two_sends_stay_two_rows(void) {
    h_reset();

    h_send_message(PEER, "one", 3, 0);
    h_send_message(PEER, "two", 3, 0);
    TEST_ASSERT_EQUAL_INT(2, msg_store_count());
    TEST_ASSERT_NOT_EQUAL(msg_store_get(0)->uid, msg_store_get(1)->uid);

    h_have_route = true;
    h_route_established(PEER);
    h_have_session = true;
    h_session_established(PEER);

    TEST_ASSERT_EQUAL_INT(2, msg_store_count());
    TEST_ASSERT_EQUAL_INT(2, h_tx_count);
    TEST_ASSERT_EQUAL_STRING("one", msg_store_get(0)->text);
    TEST_ASSERT_EQUAL_STRING("two", msg_store_get(1)->text);
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_SENT, msg_store_get(0)->status);
    TEST_ASSERT_EQUAL_INT(MSG_STATUS_SENT, msg_store_get(1)->status);
    TEST_ASSERT_NOT_EQUAL(msg_store_get(0)->packet_id, msg_store_get(1)->packet_id);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_uid_alloc_is_monotonic_and_never_zero);
    RUN_TEST(test_update_by_uid_stamps_packet_id_and_status);
    RUN_TEST(test_update_by_uid_zero_never_matches);
    RUN_TEST(test_cold_peer_send_stores_exactly_one_row);
    RUN_TEST(test_cold_session_send_stores_exactly_one_row);
    RUN_TEST(test_happy_path_stores_exactly_one_row);
    RUN_TEST(test_expired_queue_entry_fails_its_one_row);
    RUN_TEST(test_two_sends_stay_two_rows);
    return UNITY_END();
}
