#include "unity.h"
#include "../components/ui/ui_manager.c"
#include "../components/ui/ui_format.c"

static ui_state_t state;

void setUp(void) { ui_init(&state); }
void tearDown(void) {}

void test_init_main_screen(void) {
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
    TEST_ASSERT_TRUE(ui_needs_redraw(&state));
}

void test_short_press_cycles(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));

    ui_handle_button(&state, BTN_SHORT_PRESS, 2000);
    TEST_ASSERT_EQUAL(SCREEN_COMPOSE, ui_get_screen(&state));

    ui_handle_button(&state, BTN_SHORT_PRESS, 3000);
    TEST_ASSERT_EQUAL(SCREEN_NODES, ui_get_screen(&state));

    ui_handle_button(&state, BTN_SHORT_PRESS, 4000);
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));

    ui_handle_button(&state, BTN_SHORT_PRESS, 5000);
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
}

void test_double_press_back(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));

    ui_handle_button(&state, BTN_SHORT_PRESS, 2000);
    TEST_ASSERT_EQUAL(SCREEN_COMPOSE, ui_get_screen(&state));

    ui_handle_button(&state, BTN_DOUBLE_PRESS, 3000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));
}

void test_inactivity_timeout(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));

    // Not enough time
    ui_check_timeout(&state, 50000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));

    // Enough time (60s from last activity at 1000)
    ui_check_timeout(&state, 61001);
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
}

void test_screen_dirty_on_transition(void) {
    ui_mark_drawn(&state);
    TEST_ASSERT_FALSE(ui_needs_redraw(&state));

    ui_handle_button(&state, BTN_SHORT_PRESS, 1000);
    TEST_ASSERT_TRUE(ui_needs_redraw(&state));

    ui_mark_drawn(&state);
    TEST_ASSERT_FALSE(ui_needs_redraw(&state));
}

void test_long_press_dirty_no_change(void) {
    ui_mark_drawn(&state);
    ui_handle_button(&state, BTN_LONG_PRESS, 1000);
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
    TEST_ASSERT_TRUE(ui_needs_redraw(&state));
}

void test_format_main_line1(void) {
    ui_main_data_t data = { .my_addr = 0xAABBCCDD, .battery_pct = 87 };
    char buf[32];
    ui_format_main_line1(&data, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("AABBCCDD  87%", buf);
}

void test_format_uptime_seconds(void) {
    char buf[32];
    ui_format_uptime(45, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("45s", buf);
}

void test_format_uptime_minutes(void) {
    char buf[32];
    ui_format_uptime(135, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2m 15s", buf);
}

void test_format_uptime_hours(void) {
    char buf[32];
    ui_format_uptime(8100, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2h 15m", buf);
}

void test_format_uptime_days(void) {
    char buf[32];
    ui_format_uptime(100800, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1d 4h", buf);
}

void test_format_buffer_too_small(void) {
    ui_main_data_t data = { .my_addr = 0xAABBCCDD, .battery_pct = 87 };
    char buf[5];
    int ret = ui_format_main_line1(&data, buf, sizeof(buf));
    // snprintf truncates safely, ret indicates what would have been written
    TEST_ASSERT_GREATER_THAN(4, ret);
    // buf should be null-terminated and not overflow
    TEST_ASSERT_EQUAL(4, strlen(buf));
}

void test_already_on_main_no_timeout(void) {
    // Already on MAIN, timeout should not change anything
    ui_check_timeout(&state, 100000);
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_main_screen);
    RUN_TEST(test_short_press_cycles);
    RUN_TEST(test_double_press_back);
    RUN_TEST(test_inactivity_timeout);
    RUN_TEST(test_screen_dirty_on_transition);
    RUN_TEST(test_long_press_dirty_no_change);
    RUN_TEST(test_format_main_line1);
    RUN_TEST(test_format_uptime_seconds);
    RUN_TEST(test_format_uptime_minutes);
    RUN_TEST(test_format_uptime_hours);
    RUN_TEST(test_format_uptime_days);
    RUN_TEST(test_format_buffer_too_small);
    RUN_TEST(test_already_on_main_no_timeout);
    return UNITY_END();
}
