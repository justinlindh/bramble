#include "unity.h"
#include "../components/ui/ui_manager.c"
#include "../components/ui/ui_format.c"
#include "../components/ui_graphics/include/location_settings_ui.h"

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
    TEST_ASSERT_EQUAL(SCREEN_NODES, ui_get_screen(&state));

    ui_handle_button(&state, BTN_SHORT_PRESS, 3000);
    TEST_ASSERT_EQUAL(SCREEN_COMPOSE, ui_get_screen(&state));

    ui_handle_button(&state, BTN_SHORT_PRESS, 4000);
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));

    /* In settings (non-edit), short press cycles rows instead of leaving screen. */
    ui_handle_button(&state, BTN_SHORT_PRESS, 5000);
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));

    /* Double-press exits settings back to main. */
    ui_handle_button(&state, BTN_DOUBLE_PRESS, 6000);
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
}

void test_double_press_back(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));

    ui_handle_button(&state, BTN_SHORT_PRESS, 2000);
    TEST_ASSERT_EQUAL(SCREEN_NODES, ui_get_screen(&state));

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
    ui_main_data_t data = {.my_addr = 0xAABBCCDD, .battery_pct = 87};
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
    ui_main_data_t data = {.my_addr = 0xAABBCCDD, .battery_pct = 87};
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

void test_trackball_settings_row_navigation_when_not_editing(void) {
    state.current_screen = SCREEN_SETTINGS;
    state.settings_editing = false;
    state.settings_item_cursor = UI_SETTINGS_ITEM_CONN_MODE;

    ui_handle_button(&state, BTN_DOWN, 1000);
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));
    TEST_ASSERT_EQUAL(UI_SETTINGS_ITEM_OLED_ROTATION, state.settings_item_cursor);

    ui_handle_button(&state, BTN_UP, 2000);
    TEST_ASSERT_EQUAL(UI_SETTINGS_ITEM_CONN_MODE, state.settings_item_cursor);

    // Wrap up from first row -> last row
    ui_handle_button(&state, BTN_UP, 3000);
    TEST_ASSERT_EQUAL(UI_SETTINGS_ITEM_LOCATION, state.settings_item_cursor);
}

void test_trackball_select_on_settings_enters_edit_for_selected_row(void) {
    state.current_screen = SCREEN_SETTINGS;
    state.settings_editing = false;
    state.settings_item_cursor = UI_SETTINGS_ITEM_OLED_ROTATION;

    ui_handle_button(&state, BTN_SELECT, 1000);
    TEST_ASSERT_TRUE(state.settings_editing);
    TEST_ASSERT_EQUAL(UI_SETTINGS_ITEM_OLED_ROTATION, state.settings_item_cursor);
}

void test_trackball_settings_edit_navigation_connectivity_row(void) {
    state.current_screen = SCREEN_SETTINGS;
    state.settings_editing = true;
    state.settings_item_cursor = UI_SETTINGS_ITEM_CONN_MODE;
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

void test_trackball_settings_edit_navigation_oled_row(void) {
    state.current_screen = SCREEN_SETTINGS;
    state.settings_editing = true;
    state.settings_item_cursor = UI_SETTINGS_ITEM_OLED_ROTATION;
    state.settings_cursor = 0;

    ui_handle_button(&state, BTN_DOWN, 1000);
    TEST_ASSERT_EQUAL(1, state.settings_cursor);

    ui_handle_button(&state, BTN_DOWN, 2000);
    TEST_ASSERT_EQUAL(0, state.settings_cursor);

    ui_handle_button(&state, BTN_UP, 3000);
    TEST_ASSERT_EQUAL(1, state.settings_cursor);
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

void test_location_ui_actions_toggle_tier_interval(void) {
    location_ui_state_t st = {
        .sharing_enabled = false,
        .tier = LOCATION_UI_TIER_COARSE,
        .interval_s = LOCATION_UI_INTERVAL_5_MIN,
        .source = LOCATION_UI_SOURCE_HYBRID,
        .last_share_epoch_s = 0,
    };

    location_ui_apply_action(&st, LOCATION_UI_ACTION_SET_SHARING, 1);
    TEST_ASSERT_TRUE(st.sharing_enabled);

    location_ui_apply_action(&st, LOCATION_UI_ACTION_SET_TIER, LOCATION_UI_TIER_FULL);
    TEST_ASSERT_EQUAL(LOCATION_UI_TIER_FULL, st.tier);

    location_ui_apply_action(&st, LOCATION_UI_ACTION_SET_INTERVAL, LOCATION_UI_INTERVAL_15_MIN);
    TEST_ASSERT_EQUAL(LOCATION_UI_INTERVAL_15_MIN, st.interval_s);

    location_ui_apply_action(&st, LOCATION_UI_ACTION_SET_SOURCE, LOCATION_UI_SOURCE_GPS);
    TEST_ASSERT_EQUAL(LOCATION_UI_SOURCE_GPS, st.source);
}

void test_location_ui_panic_off_disables_sharing(void) {
    location_ui_state_t st = {
        .sharing_enabled = true,
        .tier = LOCATION_UI_TIER_FULL,
        .interval_s = LOCATION_UI_INTERVAL_1_MIN,
        .source = LOCATION_UI_SOURCE_GPS,
    };

    location_ui_apply_action(&st, LOCATION_UI_ACTION_PANIC_OFF, 0);
    TEST_ASSERT_FALSE(st.sharing_enabled);
    TEST_ASSERT_EQUAL(LOCATION_UI_TIER_FULL, st.tier);
    TEST_ASSERT_EQUAL(LOCATION_UI_INTERVAL_1_MIN, st.interval_s);
}

void test_location_ui_status_indicators(void) {
    char last_share[32];
    location_ui_format_last_share(last_share, sizeof(last_share), 1000, 1120);
    TEST_ASSERT_EQUAL_STRING("2m ago", last_share);

    location_ui_format_last_share(last_share, sizeof(last_share), 0, 1120);
    TEST_ASSERT_EQUAL_STRING("never", last_share);

    TEST_ASSERT_EQUAL_STRING("GPS", location_ui_source_label(LOCATION_UI_SOURCE_GPS));
    TEST_ASSERT_EQUAL_STRING("Manual", location_ui_source_label(LOCATION_UI_SOURCE_MANUAL));
    TEST_ASSERT_EQUAL_STRING("Hybrid", location_ui_source_label(LOCATION_UI_SOURCE_HYBRID));
}

void test_incoming_message_idle_auto_switches_to_messages(void) {
    state.current_screen = SCREEN_NODES;
    state.last_activity = 1000;

    ui_on_message_received(&state, 12050);

    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);
    TEST_ASSERT_EQUAL(SCREEN_NODES, state.prev_screen);
    TEST_ASSERT_FALSE(state.pending_message_notification);
    TEST_ASSERT_TRUE(state.message_auto_switch_time > 0);
}

void test_incoming_message_while_active_sets_pending_flag_without_switch(void) {
    state.current_screen = SCREEN_NODES;
    state.last_activity = 5000;

    ui_on_message_received(&state, 12000); /* 7s idle -> still active */

    TEST_ASSERT_EQUAL(SCREEN_NODES, state.current_screen);
    TEST_ASSERT_TRUE(state.pending_message_notification);
    TEST_ASSERT_EQUAL(0, state.message_auto_switch_time);
}

void test_auto_restore_returns_to_previous_screen_after_timeout(void) {
    state.current_screen = SCREEN_NODES;
    state.last_activity = 1000;
    ui_on_message_received(&state, 12050);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);

    ui_check_timeout(&state, 42000); /* not enough */
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);

    ui_check_timeout(&state, 43051); /* >30s after auto-switch */
    TEST_ASSERT_EQUAL(SCREEN_NODES, state.current_screen);
    TEST_ASSERT_EQUAL(0, state.message_auto_switch_time);
}

void test_user_interaction_on_messages_cancels_auto_restore(void) {
    state.current_screen = SCREEN_NODES;
    state.last_activity = 1000;
    ui_on_message_received(&state, 12050);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);

    ui_handle_button(&state, BTN_LONG_PRESS, 13000); /* interaction while on messages */
    TEST_ASSERT_EQUAL(0, state.message_auto_switch_time);

    ui_check_timeout(&state, 50000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);
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
    RUN_TEST(test_trackball_settings_row_navigation_when_not_editing);
    RUN_TEST(test_trackball_select_on_settings_enters_edit_for_selected_row);
    RUN_TEST(test_trackball_settings_edit_navigation_connectivity_row);
    RUN_TEST(test_trackball_settings_edit_navigation_oled_row);
    RUN_TEST(test_trackball_settings_edit_confirm_with_select);
    RUN_TEST(test_trackball_settings_edit_cancel_with_left);
    RUN_TEST(test_location_ui_actions_toggle_tier_interval);
    RUN_TEST(test_location_ui_panic_off_disables_sharing);
    RUN_TEST(test_location_ui_status_indicators);
    RUN_TEST(test_incoming_message_idle_auto_switches_to_messages);
    RUN_TEST(test_incoming_message_while_active_sets_pending_flag_without_switch);
    RUN_TEST(test_auto_restore_returns_to_previous_screen_after_timeout);
    RUN_TEST(test_user_interaction_on_messages_cancels_auto_restore);
    return UNITY_END();
}
