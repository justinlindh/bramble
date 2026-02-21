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

/* ── Trackball navigation tests ────────────────────────────────────── */

void test_trackball_right_next_screen(void) {
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
    ui_handle_button(&state, BTN_RIGHT, 1000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));
}

void test_trackball_down_next_screen(void) {
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
    ui_handle_button(&state, BTN_DOWN, 1000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));
}

void test_trackball_left_prev_screen(void) {
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
    ui_handle_button(&state, BTN_LEFT, 1000);
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));
}

void test_trackball_up_prev_screen(void) {
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
    ui_handle_button(&state, BTN_UP, 1000);
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));
}

void test_trackball_select_on_messages_opens_compose(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));
    
    ui_handle_button(&state, BTN_SELECT, 2000);
    TEST_ASSERT_EQUAL(SCREEN_COMPOSE, ui_get_screen(&state));
    TEST_ASSERT_EQUAL(0, state.compose_len);
    TEST_ASSERT_TRUE(state.compose_active);
}

void test_trackball_select_on_settings_enters_edit(void) {
    // Navigate to settings
    state.current_screen = SCREEN_SETTINGS;
    TEST_ASSERT_FALSE(state.settings_editing);
    
    ui_handle_button(&state, BTN_SELECT, 1000);
    TEST_ASSERT_TRUE(state.settings_editing);
}

void test_trackball_settings_edit_navigation(void) {
    state.current_screen = SCREEN_SETTINGS;
    state.settings_editing = true;
    state.settings_cursor = 0;
    
    // Down = next option
    ui_handle_button(&state, BTN_DOWN, 1000);
    TEST_ASSERT_EQUAL(1, state.settings_cursor);
    
    // Up = previous option
    ui_handle_button(&state, BTN_UP, 2000);
    TEST_ASSERT_EQUAL(0, state.settings_cursor);
    
    // Wrap-around test
    ui_handle_button(&state, BTN_UP, 3000);
    TEST_ASSERT_EQUAL(CONN_MODE_COUNT - 1, state.settings_cursor);
}

void test_trackball_settings_edit_confirm_with_select(void) {
    state.current_screen = SCREEN_SETTINGS;
    state.settings_editing = true;
    state.settings_confirmed = false;
    
    ui_handle_button(&state, BTN_SELECT, 1000);
    TEST_ASSERT_TRUE(state.settings_confirmed);
}

void test_trackball_settings_edit_cancel_with_left(void) {
    state.current_screen = SCREEN_SETTINGS;
    state.settings_editing = true;
    
    ui_handle_button(&state, BTN_LEFT, 1000);
    TEST_ASSERT_FALSE(state.settings_editing);
}

void test_conn_mode_resolve_boot_keeps_supported_modes(void) {
    TEST_ASSERT_EQUAL(CONN_MODE_WIFI, conn_mode_resolve_boot(CONN_MODE_WIFI, false));
    TEST_ASSERT_EQUAL(CONN_MODE_BLE, conn_mode_resolve_boot(CONN_MODE_BLE, false));
}

void test_conn_mode_resolve_boot_normalizes_legacy_both(void) {
    TEST_ASSERT_EQUAL(CONN_MODE_WIFI, conn_mode_resolve_boot(CONN_MODE_BOTH, false));
    TEST_ASSERT_EQUAL(CONN_MODE_WIFI, conn_mode_resolve_boot(CONN_MODE_BOTH, true));
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
    RUN_TEST(test_trackball_right_next_screen);
    RUN_TEST(test_trackball_down_next_screen);
    RUN_TEST(test_trackball_left_prev_screen);
    RUN_TEST(test_trackball_up_prev_screen);
    RUN_TEST(test_trackball_select_on_messages_opens_compose);
    RUN_TEST(test_conn_mode_resolve_boot_keeps_supported_modes);
    RUN_TEST(test_conn_mode_resolve_boot_normalizes_legacy_both);
    RUN_TEST(test_trackball_select_on_settings_enters_edit);
    RUN_TEST(test_trackball_settings_edit_navigation);
    RUN_TEST(test_trackball_settings_edit_confirm_with_select);
    RUN_TEST(test_trackball_settings_edit_cancel_with_left);
    return UNITY_END();
}
