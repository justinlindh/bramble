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

    stored_msg_t b_in = { .direction = MSG_DIR_BROADCAST_IN };
    stored_msg_t b_out = { .direction = MSG_DIR_BROADCAST_OUT };
    stored_msg_t dm = { .direction = MSG_DIR_INCOMING };

    TEST_ASSERT_TRUE(chat_target_matches_message(t, &b_in, -1));
    TEST_ASSERT_TRUE(chat_target_matches_message(t, &b_out, -1));
    TEST_ASSERT_FALSE(chat_target_matches_message(t, &dm, -1));
}

void test_channel_matches_only_same_channel_index(void) {
    chat_target_t t = chat_target_normalize(CHAT_TARGET_CHANNEL, 2, 4);

    stored_msg_t msg = { .direction = MSG_DIR_OUTGOING };
    TEST_ASSERT_TRUE(chat_target_matches_message(t, &msg, 2));
    TEST_ASSERT_FALSE(chat_target_matches_message(t, &msg, 1));
    TEST_ASSERT_FALSE(chat_target_matches_message(t, &msg, -1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_target_is_broadcast);
    RUN_TEST(test_normalize_invalid_channel_falls_back_to_broadcast);
    RUN_TEST(test_broadcast_matches_only_broadcast_directions);
    RUN_TEST(test_channel_matches_only_same_channel_index);
    return UNITY_END();
}
