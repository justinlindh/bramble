#include "unity.h"
#include "chat_message_ui.h"
#include "msg_store.h"
#include <stdio.h>

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

void test_details_toggle_available_for_any_outgoing_with_packet_id(void) {
    /* Gating details on DELIVERED or a recorded route made most sent
     * bubbles silently un-tappable; only outgoing + packet id matter. */
    TEST_ASSERT_TRUE(chat_message_has_details_toggle(true, 42));

    TEST_ASSERT_FALSE(chat_message_has_details_toggle(false, 42));
    TEST_ASSERT_FALSE(chat_message_has_details_toggle(true, 0));
}

static void test_name_of(char* out, size_t out_len, uint32_t addr) {
    snprintf(out, out_len, "N%02X", (unsigned)(addr & 0xFF));
}

void test_receipt_summary_lists_all_when_few(void) {
    char buf[128];
    uint32_t addrs[] = {0x11, 0x22, 0x33};
    chat_format_receipt_summary(buf, sizeof(buf), addrs, 3, 3, test_name_of);
    TEST_ASSERT_EQUAL_STRING("Delivered to 3: N11, N22, N33", buf);
}

void test_receipt_summary_truncates_with_plus_n(void) {
    char buf[128];
    uint32_t addrs[] = {0x11, 0x22, 0x33, 0x44};
    chat_format_receipt_summary(buf, sizeof(buf), addrs, 4, 9, test_name_of);
    TEST_ASSERT_EQUAL_STRING("Delivered to 9: N11, N22, N33, N44, +5", buf);
}

void test_route_line_only_for_relayed_direct_messages(void) {
    /* A relayed DM is the only case worth a route line. */
    TEST_ASSERT_TRUE(chat_message_route_is_informative(MSG_STORE_DM_CHANNEL, 3));
    TEST_ASSERT_TRUE(chat_message_route_is_informative(MSG_STORE_DM_CHANNEL, 8));

    /* Direct DM: the "route" is just sender then recipient, which restates
     * the addressing already on screen. */
    TEST_ASSERT_FALSE(chat_message_route_is_informative(MSG_STORE_DM_CHANNEL, 2));
    TEST_ASSERT_FALSE(chat_message_route_is_informative(MSG_STORE_DM_CHANNEL, 0));

    /* Channel and broadcast: the store holds one route field that every
     * recipient's receipt overwrites, so any single line would present one
     * arbitrary recipient's path as the message's path. */
    TEST_ASSERT_FALSE(chat_message_route_is_informative(0, 3));
    TEST_ASSERT_FALSE(chat_message_route_is_informative(0, 8));
    TEST_ASSERT_FALSE(chat_message_route_is_informative(2, 5));
}

void test_receipt_summary_empty_reads_no_receipts(void) {
    char buf[128];
    chat_format_receipt_summary(buf, sizeof(buf), NULL, 0, 0, test_name_of);
    TEST_ASSERT_EQUAL_STRING("No receipts yet", buf);
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
    RUN_TEST(test_details_toggle_available_for_any_outgoing_with_packet_id);
    RUN_TEST(test_receipt_summary_lists_all_when_few);
    RUN_TEST(test_receipt_summary_truncates_with_plus_n);
    RUN_TEST(test_receipt_summary_empty_reads_no_receipts);
    RUN_TEST(test_route_line_only_for_relayed_direct_messages);
    RUN_TEST(test_format_age_under_a_minute_is_now);
    RUN_TEST(test_format_age_minutes_hours_days);
    return UNITY_END();
}
