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

void test_peer_for_uid_reads_the_recipient_without_a_row_copy(void) {
    /* What arming a parked retry needs from a uid, and all it needs: parking
     * runs on the UI or RPC task, and a stored_msg_t is too big to put on
     * those stacks just to learn who a message was for. */
    msg_store_init();
    msg_store_add_dm_uid(0xC0FFEE, MSG_DIR_OUTGOING, "who", 3, 0, 0, 0, MSG_STATUS_QUEUED, 11);

    uint32_t peer = 0;
    TEST_ASSERT_TRUE(msg_store_peer_for_uid(11, &peer));
    TEST_ASSERT_EQUAL_UINT32(0xC0FFEE, peer);

    TEST_ASSERT_FALSE(msg_store_peer_for_uid(999, &peer));
    TEST_ASSERT_FALSE(msg_store_peer_for_uid(0, &peer));
    TEST_ASSERT_FALSE(msg_store_peer_for_uid(11, NULL));
    TEST_ASSERT_EQUAL_UINT32(0xC0FFEE, peer); /* a rejected read leaves it alone */
}

void test_peer_for_uid_survives_ring_wrap(void) {
    msg_store_init();
    for (int i = 0; i < 2 * MSG_STORE_MAX - 1; i++) {
        msg_store_add(0x9999, MSG_DIR_INCOMING, "filler", 6, -70, 5);
    }
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "wrap-old", 8, 0, 0, 0, MSG_STATUS_QUEUED, 100);
    msg_store_add_dm_uid(0xBBBB, MSG_DIR_OUTGOING, "wrap-new", 8, 0, 0, 0, MSG_STATUS_QUEUED, 200);

    uint32_t peer = 0;
    TEST_ASSERT_TRUE(msg_store_peer_for_uid(100, &peer));
    TEST_ASSERT_EQUAL_UINT32(0xAAAA, peer);
    TEST_ASSERT_TRUE(msg_store_peer_for_uid(200, &peer));
    TEST_ASSERT_EQUAL_UINT32(0xBBBB, peer);
}

void test_next_parked_peer_rotates_over_every_parked_peer_and_wraps(void) {
    msg_store_init();
    /* Stored out of address order on purpose: the rotation is defined by
     * address, not by insertion. */
    msg_store_add_dm_uid(0x300, MSG_DIR_OUTGOING, "c", 1, 0, 0, 0, MSG_STATUS_QUEUED, 1);
    msg_store_add_dm_uid(0x100, MSG_DIR_OUTGOING, "a", 1, 0, 0, 0, MSG_STATUS_QUEUED, 2);
    msg_store_add_dm_uid(0x200, MSG_DIR_OUTGOING, "b", 1, 0, 0, 0, MSG_STATUS_QUEUED, 3);
    msg_store_add_dm_uid(0x200, MSG_DIR_OUTGOING, "b2", 2, 0, 0, 0, MSG_STATUS_QUEUED, 4);

    uint32_t peer = 0;
    TEST_ASSERT_TRUE(msg_store_next_parked_peer(0, &peer));
    TEST_ASSERT_EQUAL_HEX32(0x100, peer);
    TEST_ASSERT_TRUE(msg_store_next_parked_peer(peer, &peer));
    TEST_ASSERT_EQUAL_HEX32(0x200, peer); /* two rows, still one turn */
    TEST_ASSERT_TRUE(msg_store_next_parked_peer(peer, &peer));
    TEST_ASSERT_EQUAL_HEX32(0x300, peer);
    TEST_ASSERT_TRUE(msg_store_next_parked_peer(peer, &peer));
    TEST_ASSERT_EQUAL_HEX32(0x100, peer); /* wrapped */
}

void test_next_parked_peer_ignores_everything_that_is_not_a_parked_dm(void) {
    msg_store_init();
    msg_store_add_dm_uid(0x100, MSG_DIR_OUTGOING, "failed", 6, 0, 0, 0, MSG_STATUS_FAILED, 1);
    msg_store_add_dm_uid(0x200, MSG_DIR_INCOMING, "in", 2, 0, 0, 0, MSG_STATUS_NONE, 2);
    msg_store_add_channel(0x300, MSG_DIR_OUTGOING, "ch", 2, 0, 0, 0, MSG_STATUS_QUEUED, 1);

    uint32_t peer = 0xDEAD;
    TEST_ASSERT_FALSE(msg_store_next_parked_peer(0, &peer));
    TEST_ASSERT_EQUAL_HEX32(0xDEAD, peer); /* untouched when nothing is parked */

    /* One real parked DM and it is found regardless of where the cursor is. */
    msg_store_add_dm_uid(0x400, MSG_DIR_OUTGOING, "parked", 6, 0, 0, 0, MSG_STATUS_QUEUED, 3);
    TEST_ASSERT_TRUE(msg_store_next_parked_peer(0, &peer));
    TEST_ASSERT_EQUAL_HEX32(0x400, peer);
    TEST_ASSERT_TRUE(msg_store_next_parked_peer(0xFFFFFFFF, &peer));
    TEST_ASSERT_EQUAL_HEX32(0x400, peer);
    TEST_ASSERT_FALSE(msg_store_next_parked_peer(0, NULL));
}

void test_next_parked_peer_survives_ring_wrap(void) {
    msg_store_init();
    for (int i = 0; i < 2 * MSG_STORE_MAX - 1; i++) {
        msg_store_add(0x9999, MSG_DIR_INCOMING, "filler", 6, -70, 5);
    }
    msg_store_add_dm_uid(0x500, MSG_DIR_OUTGOING, "late", 4, 0, 0, 0, MSG_STATUS_QUEUED, 77);

    uint32_t peer = 0;
    TEST_ASSERT_TRUE(msg_store_next_parked_peer(0, &peer));
    TEST_ASSERT_EQUAL_HEX32(0x500, peer);
}

void test_update_by_uid_refuses_queued_to_failed(void) {
    /* Parked is sticky: a send attempt failing must never silently un-park a
     * QUEUED row, or a parked message stops flushing on the next rejoin
     * after its very first failed retry. */
    msg_store_init();
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "parked", 6, 0, 0, 0, MSG_STATUS_QUEUED, 1);

    TEST_ASSERT_TRUE(msg_store_update_by_uid(1, 0, MSG_STATUS_FAILED));

    stored_msg_t out;
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(1, &out));
    TEST_ASSERT_EQUAL(MSG_STATUS_QUEUED, out.status);
}

void test_update_by_uid_allows_queued_to_sent(void) {
    /* Real progress out of QUEUED must still work: the sticky rule only
     * blocks the FAILED transition, not delivery. */
    msg_store_init();
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "parked", 6, 0, 0, 0, MSG_STATUS_QUEUED, 1);

    TEST_ASSERT_TRUE(msg_store_update_by_uid(1, 42, MSG_STATUS_SENT));

    stored_msg_t out;
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(1, &out));
    TEST_ASSERT_EQUAL(MSG_STATUS_SENT, out.status);
}

void test_update_by_uid_allows_queued_to_delivered(void) {
    msg_store_init();
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "parked", 6, 0, 0, 0, MSG_STATUS_QUEUED, 1);

    TEST_ASSERT_TRUE(msg_store_update_by_uid(1, 42, MSG_STATUS_DELIVERED));

    stored_msg_t out;
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(1, &out));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, out.status);
}

void test_update_by_uid_sent_to_failed_is_not_sticky(void) {
    /* The sticky rule must not leak to normal (non-parked) rows: a message
     * that actually reached the air and then exhausted its ACK retries is a
     * genuine failure and must still show as FAILED. */
    msg_store_init();
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "on-air", 6, 0, 0, 42, MSG_STATUS_SENT, 1);

    TEST_ASSERT_TRUE(msg_store_update_by_uid(1, 0, MSG_STATUS_FAILED));

    stored_msg_t out;
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(1, &out));
    TEST_ASSERT_EQUAL(MSG_STATUS_FAILED, out.status);
}

void test_update_status_refuses_queued_to_failed_for_a_reparked_send(void) {
    /* This pins invariant hardening, not a reachable-today code path: no
     * caller of msg_store_update_status(..., MSG_STATUS_FAILED) can
     * currently reach a row this way, since both drive off a pending-ack
     * entry that is already deactivated by the time a row can be parked
     * (see msg_store_update_status_with_route's comment). The guard is
     * pinned anyway so a future caller of this packet_id door cannot
     * quietly un-park a row the uid door already protects. */
    msg_store_init();
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "reparked", 8, 0, 0, 42, MSG_STATUS_QUEUED, 1);

    TEST_ASSERT_TRUE(msg_store_update_status(42, MSG_STATUS_FAILED));

    stored_msg_t out;
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(1, &out));
    TEST_ASSERT_EQUAL(MSG_STATUS_QUEUED, out.status);
}

void test_update_status_allows_queued_to_delivered_for_a_reparked_send(void) {
    /* A late ACK for a since-parked message really was delivered, so this
     * transition must still go through even though FAILED is refused. */
    msg_store_init();
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "reparked", 8, 0, 0, 42, MSG_STATUS_QUEUED, 1);

    TEST_ASSERT_TRUE(msg_store_update_status(42, MSG_STATUS_DELIVERED));

    stored_msg_t out;
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(1, &out));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, out.status);
}

void test_unpark_moves_queued_row_to_failed(void) {
    msg_store_init();
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "parked", 6, 0, 0, 0, MSG_STATUS_QUEUED, 1);

    TEST_ASSERT_TRUE(msg_store_unpark(1));

    stored_msg_t out;
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(1, &out));
    TEST_ASSERT_EQUAL(MSG_STATUS_FAILED, out.status);
}

void test_unpark_refuses_non_queued_or_unknown_uid(void) {
    msg_store_init();
    msg_store_add_dm_uid(0xAAAA, MSG_DIR_OUTGOING, "on-air", 6, 0, 0, 42, MSG_STATUS_SENT, 1);

    TEST_ASSERT_FALSE(msg_store_unpark(1));
    TEST_ASSERT_FALSE(msg_store_unpark(999));
    TEST_ASSERT_FALSE(msg_store_unpark(0));

    stored_msg_t out;
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(1, &out));
    TEST_ASSERT_EQUAL(MSG_STATUS_SENT, out.status);
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
    RUN_TEST(test_peer_for_uid_reads_the_recipient_without_a_row_copy);
    RUN_TEST(test_peer_for_uid_survives_ring_wrap);
    RUN_TEST(test_next_parked_peer_rotates_over_every_parked_peer_and_wraps);
    RUN_TEST(test_next_parked_peer_ignores_everything_that_is_not_a_parked_dm);
    RUN_TEST(test_next_parked_peer_survives_ring_wrap);
    RUN_TEST(test_update_by_uid_refuses_queued_to_failed);
    RUN_TEST(test_update_by_uid_allows_queued_to_sent);
    RUN_TEST(test_update_by_uid_allows_queued_to_delivered);
    RUN_TEST(test_update_by_uid_sent_to_failed_is_not_sticky);
    RUN_TEST(test_update_status_refuses_queued_to_failed_for_a_reparked_send);
    RUN_TEST(test_update_status_allows_queued_to_delivered_for_a_reparked_send);
    RUN_TEST(test_unpark_moves_queued_row_to_failed);
    RUN_TEST(test_unpark_refuses_non_queued_or_unknown_uid);
    return UNITY_END();
}
