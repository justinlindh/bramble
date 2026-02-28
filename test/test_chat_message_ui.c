#include "unity.h"
#include "chat_message_ui.h"

void setUp(void) {}
void tearDown(void) {}

void test_delivery_badge_status_sent_is_single_check_and_undelivered_color(void) {
    chat_delivery_badge_t badge = chat_message_delivery_badge(MSG_STATUS_SENT);
    TEST_ASSERT_EQUAL(CHAT_DELIVERY_BADGE_SINGLE_CHECK, badge.kind);
    TEST_ASSERT_EQUAL(CHAT_DELIVERY_COLOR_UNDELIVERED, badge.color_role);
}

void test_delivery_badge_status_delivered_is_double_check_and_delivered_color(void) {
    chat_delivery_badge_t badge = chat_message_delivery_badge(MSG_STATUS_DELIVERED);
    TEST_ASSERT_EQUAL(CHAT_DELIVERY_BADGE_DOUBLE_CHECK, badge.kind);
    TEST_ASSERT_EQUAL(CHAT_DELIVERY_COLOR_DELIVERED, badge.color_role);
}

void test_delivery_badge_status_failed_is_close_and_failed_color(void) {
    chat_delivery_badge_t badge = chat_message_delivery_badge(MSG_STATUS_FAILED);
    TEST_ASSERT_EQUAL(CHAT_DELIVERY_BADGE_FAILED, badge.kind);
    TEST_ASSERT_EQUAL(CHAT_DELIVERY_COLOR_FAILED, badge.color_role);
}

void test_route_toggle_available_only_for_outgoing_delivered_with_hops_and_packet_id(void) {
    TEST_ASSERT_TRUE(chat_message_has_inline_route_toggle(true, MSG_STATUS_DELIVERED, 2, 42));

    TEST_ASSERT_FALSE(chat_message_has_inline_route_toggle(false, MSG_STATUS_DELIVERED, 2, 42));
    TEST_ASSERT_FALSE(chat_message_has_inline_route_toggle(true, MSG_STATUS_SENT, 2, 42));
    TEST_ASSERT_FALSE(chat_message_has_inline_route_toggle(true, MSG_STATUS_DELIVERED, 1, 42));
    TEST_ASSERT_FALSE(chat_message_has_inline_route_toggle(true, MSG_STATUS_DELIVERED, 2, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_delivery_badge_status_sent_is_single_check_and_undelivered_color);
    RUN_TEST(test_delivery_badge_status_delivered_is_double_check_and_delivered_color);
    RUN_TEST(test_delivery_badge_status_failed_is_close_and_failed_color);
    RUN_TEST(test_route_toggle_available_only_for_outgoing_delivered_with_hops_and_packet_id);
    return UNITY_END();
}
