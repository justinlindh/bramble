#include "unity.h"
#include "chat_unread.h"
#include "msg_store.h"

void setUp(void) { chat_unread_reset(); }

void tearDown(void) {}

void test_incoming_broadcast_increments_channel0(void) {
    stored_msg_t msg = {
        .direction = MSG_DIR_BROADCAST_IN,
        .channel_index = -1,
    };

    chat_unread_mark_for_message(&msg);

    TEST_ASSERT_EQUAL(1, chat_unread_count_for_channel(0));
    TEST_ASSERT_EQUAL(0, chat_unread_count_for_channel(1));
}

void test_incoming_dm_increments_its_channel(void) {
    stored_msg_t msg = {
        .direction = MSG_DIR_INCOMING,
        .channel_index = 3,
    };

    chat_unread_mark_for_message(&msg);

    TEST_ASSERT_EQUAL(1, chat_unread_count_for_channel(3));
    TEST_ASSERT_EQUAL(0, chat_unread_count_for_channel(0));
}

void test_outgoing_messages_do_not_increment(void) {
    stored_msg_t dm = {
        .direction = MSG_DIR_OUTGOING,
        .channel_index = 2,
    };
    stored_msg_t broadcast = {
        .direction = MSG_DIR_BROADCAST_OUT,
        .channel_index = -1,
    };

    chat_unread_mark_for_message(&dm);
    chat_unread_mark_for_message(&broadcast);

    TEST_ASSERT_EQUAL(0, chat_unread_count_for_channel(0));
    TEST_ASSERT_EQUAL(0, chat_unread_count_for_channel(2));
}

void test_clear_only_target_channel(void) {
    stored_msg_t ch1 = {.direction = MSG_DIR_INCOMING, .channel_index = 1};
    stored_msg_t ch2 = {.direction = MSG_DIR_INCOMING, .channel_index = 2};

    chat_unread_mark_for_message(&ch1);
    chat_unread_mark_for_message(&ch2);

    chat_unread_clear_for_channel(1);

    TEST_ASSERT_EQUAL(0, chat_unread_count_for_channel(1));
    TEST_ASSERT_EQUAL(1, chat_unread_count_for_channel(2));
}

void test_invalid_channel_is_ignored(void) {
    stored_msg_t msg = {
        .direction = MSG_DIR_INCOMING,
        .channel_index = 99,
    };

    chat_unread_mark_for_message(&msg);

    TEST_ASSERT_EQUAL(0, chat_unread_count_for_channel(99));
    TEST_ASSERT_EQUAL(0, chat_unread_count_for_channel(0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_incoming_broadcast_increments_channel0);
    RUN_TEST(test_incoming_dm_increments_its_channel);
    RUN_TEST(test_outgoing_messages_do_not_increment);
    RUN_TEST(test_clear_only_target_channel);
    RUN_TEST(test_invalid_channel_is_ignored);
    return UNITY_END();
}
