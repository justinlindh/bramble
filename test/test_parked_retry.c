/* Parked-message retry trigger (main/parked_retry.c).
 *
 * The rejoin edge alone (a beacon ADMITTING an address to the neighbor table)
 * cannot reach the common case: a peer whose ACKs are being lost keeps
 * beaconing, so it never leaves the table and can never newly join it, and a
 * message parked for it would sit QUEUED forever. These tests drive the real
 * decision kernel, the real neighbor table (components/routing) and the real
 * message store; only the transmit half is faked, since mesh_task.c and
 * mesh_beacon.c are ESP-IDF-only and never host-compiled.
 */
#include "unity.h"

#include <string.h>

#include "msg_store.h"
#include "parked_retry.h"
#include "routing.h"

static neighbor_table_t s_nb;
static int s_store_scans;      /* flush attempts, i.e. scans of the message store */
static bool s_resend_succeeds; /* whether the faked transmit lands an ACK */

void setUp(void) {
    neighbor_init(&s_nb);
    msg_store_init();
    s_store_scans = 0;
    s_resend_succeeds = true;
}

void tearDown(void) {}

/* Mirrors mesh_flush_parked_for (mesh_task.c): select the peer's parked rows,
 * re-send each, and report how many rows it found. A successful re-send lands
 * as DELIVERED; a failed one reports FAILED, which msg_store's sticky rule
 * refuses, so the row stays parked exactly as it does on the device. */
static int fake_flush_parked_for(uint32_t peer_addr) {
    uint32_t uids[MSG_STORE_MAX];
    s_store_scans++;
    int n = msg_store_parked_uids_for_peer(peer_addr, uids, MSG_STORE_MAX);
    for (int i = 0; i < n; i++) {
        msg_store_update_by_uid(uids[i], (uint32_t)(0x1000 + i),
                                s_resend_succeeds ? MSG_STATUS_DELIVERED : MSG_STATUS_FAILED);
    }
    return n;
}

/* Mirrors handle_beacon (mesh_beacon.c): update the neighbor table, then run
 * the parked-flush block against the resulting is_new_peer. Returns the number
 * of parked rows the flush found, or -1 when the beacon did not flush at all. */
static int beacon_from(uint32_t peer_addr, uint32_t now_ms) {
    int before = neighbor_count(&s_nb);
    neighbor_update(&s_nb, peer_addr, -60, 8, 0xABCDu, now_ms);
    bool is_new_peer = neighbor_count(&s_nb) > before;
    if (!parked_retry_beacon_should_flush(&s_nb, peer_addr, is_new_peer, now_ms))
        return -1;
    int found = fake_flush_parked_for(peer_addr);
    parked_retry_flushed(&s_nb, peer_addr, found, now_ms);
    return found;
}

/* Mirrors mesh_park_message (mesh_task.c): move a failed outgoing DM to
 * QUEUED, then arm its peer. Returns the row's uid. */
static uint32_t park_dm_for(uint32_t peer_addr, const char* text, uint32_t now_ms) {
    uint32_t uid = msg_store_next_uid();
    msg_store_add_dm_uid(peer_addr, MSG_DIR_OUTGOING, text, strlen(text), 0, 0, 0,
                         MSG_STATUS_FAILED, uid);
    TEST_ASSERT_TRUE(msg_store_update_by_uid(uid, 0, MSG_STATUS_QUEUED));
    parked_retry_arm(&s_nb, peer_addr, now_ms);
    return uid;
}

static msg_status_t status_of(uint32_t uid) {
    stored_msg_t m;
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(uid, &m));
    return m.status;
}

/* 1. The defect. */
void test_parked_message_is_delivered_when_the_peer_never_left(void) {
    const uint32_t peer = 0xAA110011u;

    /* The peer is here and stays here: it beacons on cadence throughout, so
     * the rejoin edge fires once, now, and can never fire again. */
    TEST_ASSERT_EQUAL_INT(0, beacon_from(peer, 1000));

    uint32_t uid = park_dm_for(peer, "are you still there", 5000);
    TEST_ASSERT_EQUAL(MSG_STATUS_QUEUED, status_of(uid));

    beacon_from(peer, 65000);

    TEST_ASSERT_EQUAL_MESSAGE(MSG_STATUS_DELIVERED, status_of(uid),
                              "parked DM never went out: the peer beaconed but, having never "
                              "left the neighbor table, never rejoined it either");
}

/* 2. The cooldown. */
void test_a_stuck_peer_gets_one_attempt_per_cooldown(void) {
    const uint32_t peer = 0xBB220022u;
    s_resend_succeeds = false; /* present, but nothing gets through */

    beacon_from(peer, 1000);
    uint32_t uid = park_dm_for(peer, "still stuck", 2000);
    int before = s_store_scans;

    /* Twenty beacons in the following minute. */
    for (uint32_t t = 3000; t <= 63000; t += 3000)
        beacon_from(peer, t);

    TEST_ASSERT_EQUAL_INT_MESSAGE(before + 1, s_store_scans,
                                  "a stuck peer must not draw a flush attempt per beacon");
    TEST_ASSERT_EQUAL(MSG_STATUS_QUEUED, status_of(uid));

    /* Still not due one millisecond early, due one millisecond later. */
    beacon_from(peer, 3000 + PARKED_RETRY_COOLDOWN_MS - 1);
    TEST_ASSERT_EQUAL_INT(before + 1, s_store_scans);
    beacon_from(peer, 3000 + PARKED_RETRY_COOLDOWN_MS);
    TEST_ASSERT_EQUAL_INT(before + 2, s_store_scans);
}

/* 3a. The rejoin edge, for a peer that was never seen at all. */
void test_rejoin_edge_still_flushes_a_peer_that_was_never_in_the_table(void) {
    const uint32_t peer = 0xCC330033u;

    /* Nothing to arm: the peer has no entry. That is the case the rejoin edge
     * has always covered, and it must keep covering it. */
    uint32_t uid = park_dm_for(peer, "when you show up", 2000);
    TEST_ASSERT_FALSE(parked_retry_arm(&s_nb, peer, 2000));

    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 900000));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, status_of(uid));
}

/* 3b. The rejoin edge, for a peer that really left and came back. */
void test_rejoin_edge_still_flushes_a_peer_that_left_and_returned(void) {
    const uint32_t peer = 0xDD440044u;

    beacon_from(peer, 1000);
    uint32_t uid = park_dm_for(peer, "see you later", 2000);

    /* It goes quiet and ages out. */
    neighbor_purge(&s_nb, 1000 + NEIGHBOR_EXPIRY_MS);
    TEST_ASSERT_EQUAL_INT(0, neighbor_count(&s_nb));

    /* Coming back re-admits it, which is the rejoin edge, on a fresh entry
     * that carries no arming from before it left. */
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 1000 + NEIGHBOR_EXPIRY_MS + 1));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, status_of(uid));
}

/* 4. The no-parked-messages case, which is nearly all of the time. */
void test_a_known_peer_with_nothing_parked_never_scans_the_store(void) {
    const uint32_t peer = 0xEE550055u;

    beacon_from(peer, 1000); /* admission scans once and finds nothing */
    int before = s_store_scans;

    for (uint32_t t = 61000; t <= 61000 + 20 * 60000; t += 60000)
        TEST_ASSERT_EQUAL_INT(-1, beacon_from(peer, t));

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, s_store_scans,
                                  "a beacon from a peer with nothing parked must not touch "
                                  "the message store");
}

/* 5. Lazy disarm: a row can leave the parked state without telling anyone. */
void test_a_flush_that_finds_nothing_disarms_the_peer(void) {
    const uint32_t peer = 0x11660066u;

    beacon_from(peer, 1000);
    uint32_t uid = park_dm_for(peer, "on second thought", 2000);
    TEST_ASSERT_TRUE(msg_store_unpark(uid)); /* the user cancels it */

    /* One scan learns there is nothing left ... */
    TEST_ASSERT_EQUAL_INT(0, beacon_from(peer, 3000));
    int before = s_store_scans;

    /* ... and the peer is quiet from then on, well past a cooldown. */
    for (uint32_t t = 63000; t <= 63000 + 3600000; t += 60000)
        TEST_ASSERT_EQUAL_INT(-1, beacon_from(peer, t));
    TEST_ASSERT_EQUAL_INT(before, s_store_scans);
}

/* 6. The same lazy disarm after the message actually goes out. */
void test_delivering_the_last_parked_message_stops_the_retries(void) {
    const uint32_t peer = 0x22770077u;

    beacon_from(peer, 1000);
    uint32_t uid = park_dm_for(peer, "made it", 2000);
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 3000));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, status_of(uid));

    /* The successful flush rearmed the cooldown, so one more scan happens and
     * finds nothing. After that the peer is quiet. */
    TEST_ASSERT_EQUAL_INT(0, beacon_from(peer, 3000 + PARKED_RETRY_COOLDOWN_MS));
    int before = s_store_scans;
    for (uint32_t t = 3000 + PARKED_RETRY_COOLDOWN_MS + 60000; t <= 10000000; t += 60000)
        TEST_ASSERT_EQUAL_INT(-1, beacon_from(peer, t));
    TEST_ASSERT_EQUAL_INT(before, s_store_scans);
}

/* 7. A fresh park is fresh user intent and does not wait out the cooldown. */
void test_parking_another_message_does_not_wait_out_the_cooldown(void) {
    const uint32_t peer = 0x33880088u;
    s_resend_succeeds = false;

    beacon_from(peer, 1000);
    park_dm_for(peer, "first", 2000);
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 3000)); /* attempt, stays parked */
    TEST_ASSERT_EQUAL_INT(-1, beacon_from(peer, 4000)); /* cooling down */

    s_resend_succeeds = true;
    uint32_t uid2 = park_dm_for(peer, "second", 5000);
    TEST_ASSERT_EQUAL_INT(2, beacon_from(peer, 6000));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, status_of(uid2));
}

/* 8. Arming a peer that is not in the table is a no-op, not a crash. */
void test_arming_an_absent_peer_is_a_no_op(void) {
    TEST_ASSERT_FALSE(parked_retry_arm(&s_nb, 0x44990099u, 1000));
    TEST_ASSERT_EQUAL_INT(0, neighbor_count(&s_nb));
}

/* 9. now_ms is a wrapping 32-bit uptime (mesh_task.c now_ms), so a cooldown
 * that straddles the wrap must still expire. */
void test_the_cooldown_survives_the_uptime_clock_wrapping(void) {
    const uint32_t peer = 0x5AA0AA00u;
    const uint32_t near_wrap = 0xFFFFF000u;
    s_resend_succeeds = false;

    beacon_from(peer, near_wrap);
    park_dm_for(peer, "across the wrap", near_wrap + 100);
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, near_wrap + 200));

    /* The rearmed deadline is near_wrap + 200 + PARKED_RETRY_COOLDOWN_MS,
     * which wraps past zero. */
    uint32_t due = near_wrap + 200 + PARKED_RETRY_COOLDOWN_MS;
    TEST_ASSERT_TRUE(due < near_wrap); /* it really did wrap */
    TEST_ASSERT_EQUAL_INT(-1, beacon_from(peer, due - 1));
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, due));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parked_message_is_delivered_when_the_peer_never_left);
    RUN_TEST(test_a_stuck_peer_gets_one_attempt_per_cooldown);
    RUN_TEST(test_rejoin_edge_still_flushes_a_peer_that_was_never_in_the_table);
    RUN_TEST(test_rejoin_edge_still_flushes_a_peer_that_left_and_returned);
    RUN_TEST(test_a_known_peer_with_nothing_parked_never_scans_the_store);
    RUN_TEST(test_a_flush_that_finds_nothing_disarms_the_peer);
    RUN_TEST(test_delivering_the_last_parked_message_stops_the_retries);
    RUN_TEST(test_parking_another_message_does_not_wait_out_the_cooldown);
    RUN_TEST(test_arming_an_absent_peer_is_a_no_op);
    RUN_TEST(test_the_cooldown_survives_the_uptime_clock_wrapping);
    return UNITY_END();
}
