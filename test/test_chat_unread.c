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

void test_incoming_dm_without_channel_counts_by_peer(void) {
    stored_msg_t msg = {
        .direction = MSG_DIR_INCOMING,
        .channel_index = -1,
        .peer_addr = 0xA1B2C3D4,
    };

    chat_unread_mark_for_message(&msg);
    chat_unread_mark_for_message(&msg);

    TEST_ASSERT_EQUAL(2, chat_unread_count_for_dm(0xA1B2C3D4));
    TEST_ASSERT_EQUAL(0, chat_unread_count_for_dm(0x11111111));
    /* DM traffic must not leak into channel counters */
    TEST_ASSERT_EQUAL(0, chat_unread_count_for_channel(0));
}

void test_clear_dm_only_clears_that_peer(void) {
    stored_msg_t a = {.direction = MSG_DIR_INCOMING, .channel_index = -1, .peer_addr = 0xAAAA0001};
    stored_msg_t b = {.direction = MSG_DIR_INCOMING, .channel_index = -1, .peer_addr = 0xBBBB0002};
    chat_unread_mark_for_message(&a);
    chat_unread_mark_for_message(&b);

    chat_unread_clear_for_dm(0xAAAA0001);

    TEST_ASSERT_EQUAL(0, chat_unread_count_for_dm(0xAAAA0001));
    TEST_ASSERT_EQUAL(1, chat_unread_count_for_dm(0xBBBB0002));
}

void test_dm_peer_table_caps_at_eight(void) {
    for (uint32_t i = 0; i < 10; i++) {
        stored_msg_t m = {
            .direction = MSG_DIR_INCOMING, .channel_index = -1, .peer_addr = 0x1000 + i};
        chat_unread_mark_for_message(&m);
    }
    /* First eight tracked, rest dropped */
    TEST_ASSERT_EQUAL(1, chat_unread_count_for_dm(0x1000));
    TEST_ASSERT_EQUAL(1, chat_unread_count_for_dm(0x1007));
    TEST_ASSERT_EQUAL(0, chat_unread_count_for_dm(0x1008));
}

void test_reset_clears_dm_counts(void) {
    stored_msg_t m = {.direction = MSG_DIR_INCOMING, .channel_index = -1, .peer_addr = 0xCAFE0001};
    chat_unread_mark_for_message(&m);
    chat_unread_reset();
    TEST_ASSERT_EQUAL(0, chat_unread_count_for_dm(0xCAFE0001));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_incoming_broadcast_increments_channel0);
    RUN_TEST(test_incoming_dm_increments_its_channel);
    RUN_TEST(test_outgoing_messages_do_not_increment);
    RUN_TEST(test_clear_only_target_channel);
    RUN_TEST(test_invalid_channel_is_ignored);
    RUN_TEST(test_incoming_dm_without_channel_counts_by_peer);
    RUN_TEST(test_clear_dm_only_clears_that_peer);
    RUN_TEST(test_dm_peer_table_caps_at_eight);
    RUN_TEST(test_reset_clears_dm_counts);
    return UNITY_END();
}
