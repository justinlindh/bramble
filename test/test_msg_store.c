#include "unity.h"
#include "msg_store.h"
#include <stdio.h>
#include <string.h>

void setUp(void) { msg_store_init(); }
void tearDown(void) {}

void test_msg_store_default_channel_index_is_minus_one(void) {
    msg_store_add_ex(0x1234, MSG_DIR_OUTGOING, "hi", 2, 0, 0, 1, MSG_STATUS_SENT);
    const stored_msg_t* m = msg_store_get(0);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL(-1, m->channel_index);
}

void test_msg_store_add_ex2_persists_channel_index(void) {
    msg_store_add_ex2(0x5678, MSG_DIR_INCOMING, "hey", 3, -80, 7, 2, MSG_STATUS_NONE, 3);
    const stored_msg_t* m = msg_store_get(0);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL(3, m->channel_index);
}

void test_msg_store_add_dm_stores_dm_channel(void) {
    /* A DM (and any channel-less message) must store MSG_STORE_DM_CHANNEL, the
     * negative marker the DM thread and unread filters key on. Forcing it here
     * is the point: no caller can hand a stray channel index (bug F1). */
    msg_store_add_dm(0x1234, MSG_DIR_OUTGOING, "dm", 2, 0, 0, 7, MSG_STATUS_SENT);
    const stored_msg_t* m = msg_store_get(0);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL(MSG_STORE_DM_CHANNEL, m->channel_index);
    TEST_ASSERT_TRUE(m->channel_index < 0);
}

void test_msg_store_add_channel_stores_nonnegative_index(void) {
    /* A channel message stores its non-negative index (channel 0 is the
     * broadcast channel). The uint8_t parameter makes a negative "DM" index
     * unrepresentable at the call site, so channel traffic can never be filed
     * channel-less and mistaken for a DM. */
    msg_store_add_channel(0xAAAA, MSG_DIR_INCOMING, "ch", 2, -70, 5, 0, MSG_STATUS_NONE, 3);
    const stored_msg_t* m = msg_store_get(0);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL(3, m->channel_index);
    TEST_ASSERT_TRUE(m->channel_index >= 0);

    msg_store_add_channel(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT, "b", 1, 0, 0, 0, MSG_STATUS_NONE, 0);
    const stored_msg_t* b = msg_store_get(1);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL(0, b->channel_index);
    TEST_ASSERT_TRUE(b->channel_index >= 0);
}

void test_total_incoming_is_monotonic_and_ignores_outgoing(void) {
    msg_store_init();
    TEST_ASSERT_EQUAL_UINT32(0, msg_store_total_incoming());

    msg_store_add(0x11111111, MSG_DIR_INCOMING, "hi", 2, -70, 5);
    TEST_ASSERT_EQUAL_UINT32(1, msg_store_total_incoming());

    msg_store_add(0x22222222, MSG_DIR_OUTGOING, "yo", 2, 0, 0);
    msg_store_add(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT, "b", 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(1, msg_store_total_incoming());

    msg_store_add(0x33333333, MSG_DIR_BROADCAST_IN, "bc", 2, -80, 3);
    TEST_ASSERT_EQUAL_UINT32(2, msg_store_total_incoming());

    /* Keep counting past the 20-slot ring capacity (the whole point). */
    for (int i = 0; i < 25; i++) {
        msg_store_add(0x44444444, MSG_DIR_INCOMING, "x", 1, -80, 3);
    }
    TEST_ASSERT_EQUAL_UINT32(27, msg_store_total_incoming());
    TEST_ASSERT_EQUAL(MSG_STORE_MAX, msg_store_count());
}

void test_ring_keeps_newest_window_at_capacity(void) {
    msg_store_init();
    char text[16];
    for (int i = 0; i < MSG_STORE_MAX + 5; i++) {
        snprintf(text, sizeof(text), "m%d", i);
        msg_store_add(0x1111, MSG_DIR_INCOMING, text, strlen(text), -70, 5);
    }
    TEST_ASSERT_EQUAL(MSG_STORE_MAX, msg_store_count());
    /* Oldest retained message is number 5; newest is MSG_STORE_MAX + 4 */
    char expect[16];
    snprintf(expect, sizeof(expect), "m%d", 5);
    TEST_ASSERT_EQUAL_STRING(expect, msg_store_get(0)->text);
    snprintf(expect, sizeof(expect), "m%d", MSG_STORE_MAX + 4);
    TEST_ASSERT_EQUAL_STRING(expect, msg_store_get(msg_store_count() - 1)->text);
}

void test_get_copy_snapshots_message_and_bounds_check(void) {
    msg_store_init();

    stored_msg_t out;
    /* Empty store: any index is out of range. */
    TEST_ASSERT_FALSE(msg_store_get_copy(0, &out));
    TEST_ASSERT_FALSE(msg_store_get_copy(-1, &out));
    /* NULL destination is rejected, not dereferenced. */
    TEST_ASSERT_FALSE(msg_store_get_copy(0, NULL));

    msg_store_add_ex2(0xABCD, MSG_DIR_INCOMING, "hello", 5, -60, 9, 42, MSG_STATUS_NONE, 4);

    TEST_ASSERT_TRUE(msg_store_get_copy(0, &out));
    TEST_ASSERT_EQUAL_STRING("hello", out.text);
    TEST_ASSERT_EQUAL_UINT32(0xABCD, out.peer_addr);
    TEST_ASSERT_EQUAL(4, out.channel_index);
    TEST_ASSERT_EQUAL_UINT32(42, out.packet_id);
    TEST_ASSERT_EQUAL(5, out.text_len);

    /* One past the end is rejected. */
    TEST_ASSERT_FALSE(msg_store_get_copy(msg_store_count(), &out));

    /* The copy is independent: a later add that evicts the slot must not
     * mutate the caller's snapshot. */
    for (int i = 0; i < MSG_STORE_MAX + 2; i++) {
        msg_store_add(0x1, MSG_DIR_INCOMING, "x", 1, 0, 0);
    }
    TEST_ASSERT_EQUAL_STRING("hello", out.text);
}

void test_count_outgoing_delivered_tracks_receipted_dms_only(void) {
    msg_store_init();
    TEST_ASSERT_EQUAL_UINT32(0, msg_store_count_outgoing_delivered());

    /* Outgoing DM, transmitted but not yet receipted: not counted. */
    msg_store_add_ex(0x1111, MSG_DIR_OUTGOING, "dm1", 3, 0, 0, 101, MSG_STATUS_SENT);
    TEST_ASSERT_EQUAL_UINT32(0, msg_store_count_outgoing_delivered());

    /* The delivery receipt arrives: counted. */
    TEST_ASSERT_TRUE(msg_store_update_status(101, MSG_STATUS_DELIVERED));
    TEST_ASSERT_EQUAL_UINT32(1, msg_store_count_outgoing_delivered());

    /* Incoming and broadcast rows never count, whatever their status. */
    msg_store_add(0x2222, MSG_DIR_INCOMING, "in", 2, -70, 5);
    msg_store_add_ex(0xFFFFFFFF, MSG_DIR_BROADCAST_OUT, "b", 1, 0, 0, 102, MSG_STATUS_SENT);
    TEST_ASSERT_EQUAL_UINT32(1, msg_store_count_outgoing_delivered());

    /* A failed DM (retries exhausted) is not a delivery. */
    msg_store_add_ex(0x3333, MSG_DIR_OUTGOING, "dm2", 3, 0, 0, 103, MSG_STATUS_SENT);
    TEST_ASSERT_TRUE(msg_store_update_status(103, MSG_STATUS_FAILED));
    TEST_ASSERT_EQUAL_UINT32(1, msg_store_count_outgoing_delivered());
}

void test_parked_uids_selects_only_this_peers_parked_outgoing_dms(void) {
    msg_store_init();
    /* Oldest first in insertion order. */
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "one", 3, 0, 0, 0, MSG_STATUS_QUEUED, 1);
    msg_store_add_dm_uid(0xBBBB, MSG_DIR_OUTGOING, "other peer", 10, 0, 0, 0, MSG_STATUS_QUEUED, 2);
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_INCOMING, "incoming", 8, 0, 0, 0, MSG_STATUS_QUEUED, 3);
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "failed", 6, 0, 0, 0, MSG_STATUS_FAILED, 4);
    msg_store_add_channel(0xAAAA, MSG_DIR_OUTGOING, "channel", 7, 0, 0, 0, MSG_STATUS_QUEUED, 0);
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "two", 3, 0, 0, 0, MSG_STATUS_QUEUED, 5);

    uint32_t uids[8];
    int n = msg_store_parked_uids_for_peer(0xAAAA, uids, 8);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_UINT32(1, uids[0]);
    TEST_ASSERT_EQUAL_UINT32(5, uids[1]);
}

void test_parked_uids_respects_the_output_bound(void) {
    msg_store_init();
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "a", 1, 0, 0, 0, MSG_STATUS_QUEUED, 1);
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "b", 1, 0, 0, 0, MSG_STATUS_QUEUED, 2);

    uint32_t uids[1];
    TEST_ASSERT_EQUAL_INT(1, msg_store_parked_uids_for_peer(0xAAAA, uids, 1));
    TEST_ASSERT_EQUAL_UINT32(1, uids[0]);
}

void test_parked_uids_empty_when_nothing_is_parked(void) {
    msg_store_init();
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "sent", 4, 0, 0, 0, MSG_STATUS_SENT, 1);
    uint32_t uids[4];
    TEST_ASSERT_EQUAL_INT(0, msg_store_parked_uids_for_peer(0xAAAA, uids, 4));
    TEST_ASSERT_EQUAL_INT(0, msg_store_parked_uids_for_peer(0xAAAA, NULL, 4));
    TEST_ASSERT_EQUAL_INT(0, msg_store_parked_uids_for_peer(0xAAAA, uids, 0));
}

void test_parked_uids_survive_ring_wrap(void) {
    msg_store_init();
    /* Push filler rows through the ring so it wraps at least once, landing the
     * two target rows on opposite sides of the physical wrap boundary
     * (physical index MSG_STORE_MAX - 1, then back to physical index 0). A
     * walk over the raw array (s_msgs[0..count)) instead of from the ring's
     * logical start would read the newer row (uid 200, physical 0) before the
     * older one (uid 100, physical MSG_STORE_MAX - 1): exactly backwards. */
    for (int i = 0; i < 2 * MSG_STORE_MAX - 1; i++) {
        msg_store_add(0x9999, MSG_DIR_INCOMING, "filler", 6, -70, 5);
    }
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "wrap-old", 8, 0, 0, 0, MSG_STATUS_QUEUED, 100);
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "wrap-new", 8, 0, 0, 0, MSG_STATUS_QUEUED, 200);

    uint32_t uids[4];
    int n = msg_store_parked_uids_for_peer(0xAAAA, uids, 4);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_UINT32(100, uids[0]);
    TEST_ASSERT_EQUAL_UINT32(200, uids[1]);
}

void test_get_copy_by_uid_finds_row_and_rejects_unknown_or_zero(void) {
    msg_store_init();
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "hello", 5, 0, 0, 0, MSG_STATUS_QUEUED, 7);

    stored_msg_t out;
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(7, &out));
    TEST_ASSERT_EQUAL_STRING("hello", out.text);
    TEST_ASSERT_EQUAL_UINT32(0xAAAA, out.peer_addr);

    TEST_ASSERT_FALSE(msg_store_get_copy_by_uid(999, &out));
    TEST_ASSERT_FALSE(msg_store_get_copy_by_uid(0, &out));
    TEST_ASSERT_FALSE(msg_store_get_copy_by_uid(7, NULL));
}

void test_get_copy_by_uid_survives_ring_wrap(void) {
    msg_store_init();
    for (int i = 0; i < 2 * MSG_STORE_MAX - 1; i++) {
        msg_store_add(0x9999, MSG_DIR_INCOMING, "filler", 6, -70, 5);
    }
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "wrap-old", 8, 0, 0, 0, MSG_STATUS_QUEUED, 100);
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "wrap-new", 8, 0, 0, 0, MSG_STATUS_QUEUED, 200);

    stored_msg_t out;
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(100, &out));
    TEST_ASSERT_EQUAL_STRING("wrap-old", out.text);
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(200, &out));
    TEST_ASSERT_EQUAL_STRING("wrap-new", out.text);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_msg_store_default_channel_index_is_minus_one);
    RUN_TEST(test_msg_store_add_ex2_persists_channel_index);
    RUN_TEST(test_msg_store_add_dm_stores_dm_channel);
    RUN_TEST(test_msg_store_add_channel_stores_nonnegative_index);
    RUN_TEST(test_total_incoming_is_monotonic_and_ignores_outgoing);
    RUN_TEST(test_ring_keeps_newest_window_at_capacity);
    RUN_TEST(test_get_copy_snapshots_message_and_bounds_check);
    RUN_TEST(test_count_outgoing_delivered_tracks_receipted_dms_only);
    RUN_TEST(test_parked_uids_selects_only_this_peers_parked_outgoing_dms);
    RUN_TEST(test_parked_uids_respects_the_output_bound);
    RUN_TEST(test_parked_uids_empty_when_nothing_is_parked);
    RUN_TEST(test_parked_uids_survive_ring_wrap);
    RUN_TEST(test_get_copy_by_uid_finds_row_and_rejects_unknown_or_zero);
    RUN_TEST(test_get_copy_by_uid_survives_ring_wrap);
    return UNITY_END();
}
