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

    stored_msg_t dm_match = {
        .direction = MSG_DIR_INCOMING, .peer_addr = 0x12345678, .channel_index = 0};
    stored_msg_t dm_other = {
        .direction = MSG_DIR_INCOMING, .peer_addr = 0xAABBCCDD, .channel_index = 0};
    stored_msg_t bcast = {
        .direction = MSG_DIR_BROADCAST_IN, .peer_addr = 0x12345678, .channel_index = 0};

    TEST_ASSERT_TRUE(chat_target_matches_message(t, &dm_match, 0));
    TEST_ASSERT_FALSE(chat_target_matches_message(t, &dm_other, 0));
    TEST_ASSERT_FALSE(chat_target_matches_message(t, &bcast, 0));
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
    RUN_TEST(test_cycle_targets_walks_channels_then_wraps_to_broadcast);
    RUN_TEST(test_cycle_from_dm_returns_broadcast);
    return UNITY_END();
}
