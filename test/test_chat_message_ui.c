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

void test_format_age_under_a_minute_is_now(void) {
    char buf[8];
    chat_format_age(45, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("now", buf);
}

void test_format_age_minutes_hours_days(void) {
    char buf[8];
    chat_format_age(300, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("5m", buf);
    chat_format_age(7200, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2h", buf);
    chat_format_age(172800, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2d", buf);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_delivery_badge_status_sent_is_single_check_and_undelivered_color);
    RUN_TEST(test_delivery_badge_status_delivered_is_double_check_and_delivered_color);
    RUN_TEST(test_delivery_badge_status_failed_is_close_and_failed_color);
    RUN_TEST(test_route_toggle_available_only_for_outgoing_delivered_with_hops_and_packet_id);
    RUN_TEST(test_format_age_under_a_minute_is_now);
    RUN_TEST(test_format_age_minutes_hours_days);
    return UNITY_END();
}
