#include "unity.h"
#include "../components/ui_graphics/include/chat_target.h"
#include "../components/msg_store/include/msg_store.h"

void setUp(void) {}
void tearDown(void) {}

void test_default_target_is_broadcast(void) {
    chat_target_t t = chat_target_default();
    TEST_ASSERT_EQUAL(CHAT_TARGET_BROADCAST, t.kind);
    TEST_ASSERT_EQUAL(-1, t.channel_index);
}

void test_normalize_invalid_channel_falls_back_to_broadcast(void) {
    chat_target_t t = chat_target_normalize(CHAT_TARGET_CHANNEL, 99, 3);
    TEST_ASSERT_EQUAL(CHAT_TARGET_BROADCAST, t.kind);
    TEST_ASSERT_EQUAL(-1, t.channel_index);
}

void test_broadcast_matches_only_broadcast_directions(void) {
    chat_target_t t = chat_target_default();

    stored_msg_t b_in = {.direction = MSG_DIR_BROADCAST_IN, .channel_index = 0};
    stored_msg_t b_out = {.direction = MSG_DIR_BROADCAST_OUT, .channel_index = 0};
    stored_msg_t dm = {.direction = MSG_DIR_INCOMING, .channel_index = 0};

    TEST_ASSERT_TRUE(chat_target_matches_message(t, &b_in, 0));
    TEST_ASSERT_TRUE(chat_target_matches_message(t, &b_out, 0));
    TEST_ASSERT_FALSE(chat_target_matches_message(t, &dm, 0));
}

void test_channel_target_excludes_other_channels(void) {
    chat_target_t t = chat_target_normalize(CHAT_TARGET_CHANNEL, 2, 4);

    stored_msg_t ch1 = {.direction = MSG_DIR_BROADCAST_IN, .channel_index = 1};
    TEST_ASSERT_FALSE(chat_target_matches_message(t, &ch1, 1));
}

void test_channel_target_includes_message_when_channel_matches(void) {
    chat_target_t t = chat_target_normalize(CHAT_TARGET_CHANNEL, 2, 4);

    stored_msg_t m = {.direction = MSG_DIR_BROADCAST_IN, .channel_index = 2};
    TEST_ASSERT_TRUE(chat_target_matches_message(t, &m, 2));
}

void test_dm_target_matches_only_peer_dm(void) {
    chat_target_t t = chat_target_dm(0x12345678);

    /* Real DMs are stored with channel_index -1 */
    stored_msg_t dm_match = {
        .direction = MSG_DIR_INCOMING, .peer_addr = 0x12345678, .channel_index = -1};
    stored_msg_t dm_other = {
        .direction = MSG_DIR_INCOMING, .peer_addr = 0xAABBCCDD, .channel_index = -1};
    stored_msg_t bcast = {
        .direction = MSG_DIR_BROADCAST_IN, .peer_addr = 0x12345678, .channel_index = -1};

    TEST_ASSERT_TRUE(chat_target_matches_message(t, &dm_match, -1));
    TEST_ASSERT_FALSE(chat_target_matches_message(t, &dm_other, -1));
    TEST_ASSERT_FALSE(chat_target_matches_message(t, &bcast, -1));
}

void test_dm_target_excludes_channel_messages_from_same_peer(void) {
    chat_target_t t = chat_target_dm(0x12345678);

    /* Channel messages are stored MSG_DIR_INCOMING with channel_index >= 0;
     * a channel post from the peer must not leak into the DM thread. */
    stored_msg_t ch_post = {
        .direction = MSG_DIR_INCOMING, .peer_addr = 0x12345678, .channel_index = 2};
    stored_msg_t ch_out = {
        .direction = MSG_DIR_OUTGOING, .peer_addr = 0x12345678, .channel_index = 2};

    TEST_ASSERT_FALSE(chat_target_matches_message(t, &ch_post, 2));
    TEST_ASSERT_FALSE(chat_target_matches_message(t, &ch_out, 2));
}

/* Regression (nav review F1): a received DM arrives on channel_id 0 (only
 * channel_id > 0 is a real channel message). Storing that raw 0 as the
 * channel_index made every incoming DM fail this filter's `< 0` test, so it
 * never rendered in its own thread. Lock the rx convention against the filter
 * that consumes it: the two must agree or DMs go invisible again. */
void test_rx_channel_index_marks_dms_as_channel_less(void) {
    TEST_ASSERT_EQUAL(-1, msg_store_rx_channel_index(0)); /* DM: no channel */
    TEST_ASSERT_EQUAL(1, msg_store_rx_channel_index(1));  /* real channel */
    TEST_ASSERT_EQUAL(7, msg_store_rx_channel_index(7));
}

void test_incoming_dm_stored_by_rx_convention_renders_in_dm_thread(void) {
    chat_target_t t = chat_target_dm(0x12345678);

    /* Exactly what mesh_task stores for a DM received on channel_id 0. */
    stored_msg_t rx_dm = {
        .direction = MSG_DIR_INCOMING,
        .peer_addr = 0x12345678,
        .channel_index = msg_store_rx_channel_index(0),
    };

    TEST_ASSERT_TRUE(chat_target_matches_message(t, &rx_dm, rx_dm.channel_index));

    /* And a channel post received on channel 2 still stays out of the thread. */
    stored_msg_t rx_ch = {
        .direction = MSG_DIR_INCOMING,
        .peer_addr = 0x12345678,
        .channel_index = msg_store_rx_channel_index(2),
    };
    TEST_ASSERT_FALSE(chat_target_matches_message(t, &rx_ch, rx_ch.channel_index));
}

void test_cycle_targets_walks_channels_then_wraps_to_broadcast(void) {
    chat_target_t t = chat_target_default();

    t = chat_target_cycle(t, 3);
    TEST_ASSERT_EQUAL(CHAT_TARGET_CHANNEL, t.kind);
    TEST_ASSERT_EQUAL(1, t.channel_index);

    t = chat_target_cycle(t, 3);
    TEST_ASSERT_EQUAL(CHAT_TARGET_CHANNEL, t.kind);
    TEST_ASSERT_EQUAL(2, t.channel_index);

    t = chat_target_cycle(t, 3);
    TEST_ASSERT_EQUAL(CHAT_TARGET_BROADCAST, t.kind);
    TEST_ASSERT_EQUAL(-1, t.channel_index);
}

void test_cycle_from_dm_returns_broadcast(void) {
    chat_target_t t = chat_target_dm(0x12345678);
    t = chat_target_cycle(t, 3);
    TEST_ASSERT_EQUAL(CHAT_TARGET_BROADCAST, t.kind);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_target_is_broadcast);
    RUN_TEST(test_normalize_invalid_channel_falls_back_to_broadcast);
    RUN_TEST(test_broadcast_matches_only_broadcast_directions);
    RUN_TEST(test_channel_target_excludes_other_channels);
    RUN_TEST(test_channel_target_includes_message_when_channel_matches);
    RUN_TEST(test_dm_target_matches_only_peer_dm);
    RUN_TEST(test_dm_target_excludes_channel_messages_from_same_peer);
    RUN_TEST(test_rx_channel_index_marks_dms_as_channel_less);
    RUN_TEST(test_incoming_dm_stored_by_rx_convention_renders_in_dm_thread);
    RUN_TEST(test_cycle_targets_walks_channels_then_wraps_to_broadcast);
    RUN_TEST(test_cycle_from_dm_returns_broadcast);
    return UNITY_END();
}
