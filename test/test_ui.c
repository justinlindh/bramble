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
    /* Non-messages screens bounce to MAIN after 60s. */
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000);
    ui_handle_button(&state, BTN_SHORT_PRESS, 1100); /* -> SCREEN_NODES */
    TEST_ASSERT_EQUAL(SCREEN_NODES, ui_get_screen(&state));

    ui_check_timeout(&state, 50000);
    TEST_ASSERT_EQUAL(SCREEN_NODES, ui_get_screen(&state));

    ui_check_timeout(&state, 61101);
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
}

void test_messages_screen_gets_long_inactivity_timeout(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000); /* -> SCREEN_MESSAGES */
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));

    /* 60s inactivity must NOT bounce a reader off the messages screen. */
    ui_check_timeout(&state, 62000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));

    /* But 5 minutes does. */
    ui_check_timeout(&state, 302000);
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
    char buf[5];
    // "1d 4h" is 5 chars; a 5-byte buffer forces snprintf to truncate.
    int ret = ui_format_uptime(100800, buf, sizeof(buf));
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

void test_gps_screen_skipped_by_default(void) {
    /* gps_available defaults to false (ui_init zeroes the struct); short-press
     * cycling must go straight from Stats to Settings without landing on GPS. */
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000); /* MESSAGES */
    ui_handle_button(&state, BTN_SHORT_PRESS, 2000); /* NODES */
    ui_handle_button(&state, BTN_SHORT_PRESS, 3000); /* COMPOSE */
    ui_handle_button(&state, BTN_SHORT_PRESS, 4000);
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));
}

void test_gps_screen_reachable_when_available(void) {
    ui_set_gps_available(&state, true);

    ui_handle_button(&state, BTN_SHORT_PRESS, 1000); /* MESSAGES */
    ui_handle_button(&state, BTN_SHORT_PRESS, 2000); /* NODES */
    ui_handle_button(&state, BTN_SHORT_PRESS, 3000); /* COMPOSE */
    ui_handle_button(&state, BTN_SHORT_PRESS, 4000);
    TEST_ASSERT_EQUAL(SCREEN_GPS, ui_get_screen(&state));

    ui_handle_button(&state, BTN_SHORT_PRESS, 5000);
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));
}

void test_gps_screen_skipped_going_backward_when_unavailable(void) {
    /* From Settings, trackball-left should land on Stats (COMPOSE), not GPS. */
    ui_handle_button(&state, BTN_LEFT, 1000);
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));

    ui_handle_button(&state, BTN_LEFT, 2000);
    TEST_ASSERT_EQUAL(SCREEN_COMPOSE, ui_get_screen(&state));
}

void test_gps_screen_reachable_going_backward_when_available(void) {
    ui_set_gps_available(&state, true);

    ui_handle_button(&state, BTN_LEFT, 1000);
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));

    ui_handle_button(&state, BTN_LEFT, 2000);
    TEST_ASSERT_EQUAL(SCREEN_GPS, ui_get_screen(&state));
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

void test_settings_gps_row_skipped_when_unavailable(void) {
    /* gps_available defaults false: row cycling must never land on the GPS row. */
    state.current_screen = SCREEN_SETTINGS;
    state.settings_editing = false;
    state.settings_item_cursor = UI_SETTINGS_ITEM_CONN_MODE;

    ui_handle_button(&state, BTN_DOWN, 1000);
    TEST_ASSERT_EQUAL(UI_SETTINGS_ITEM_OLED_ROTATION, state.settings_item_cursor);
    ui_handle_button(&state, BTN_DOWN, 2000);
    TEST_ASSERT_EQUAL(UI_SETTINGS_ITEM_LOCATION, state.settings_item_cursor);
    /* Next wraps back to CONN_MODE, hopping over the hidden GPS row. */
    ui_handle_button(&state, BTN_DOWN, 3000);
    TEST_ASSERT_EQUAL(UI_SETTINGS_ITEM_CONN_MODE, state.settings_item_cursor);
}

void test_settings_gps_row_reachable_when_available(void) {
    ui_set_gps_available(&state, true);
    state.current_screen = SCREEN_SETTINGS;
    state.settings_editing = false;
    state.settings_item_cursor = UI_SETTINGS_ITEM_LOCATION;

    /* From Location, next lands on GPS, then wraps to CONN_MODE. */
    ui_handle_button(&state, BTN_DOWN, 1000);
    TEST_ASSERT_EQUAL(UI_SETTINGS_ITEM_GPS, state.settings_item_cursor);
    ui_handle_button(&state, BTN_DOWN, 2000);
    TEST_ASSERT_EQUAL(UI_SETTINGS_ITEM_CONN_MODE, state.settings_item_cursor);
    /* And backward from CONN_MODE reaches GPS. */
    ui_handle_button(&state, BTN_UP, 3000);
    TEST_ASSERT_EQUAL(UI_SETTINGS_ITEM_GPS, state.settings_item_cursor);
}

void test_settings_gps_edit_toggles_off_and_on(void) {
    ui_set_gps_available(&state, true);
    state.current_screen = SCREEN_SETTINGS;
    state.settings_editing = true;
    state.settings_item_cursor = UI_SETTINGS_ITEM_GPS;
    state.settings_cursor = 0;

    /* Two options (Off / On): down advances, then wraps back. */
    ui_handle_button(&state, BTN_DOWN, 1000);
    TEST_ASSERT_EQUAL(1, state.settings_cursor);
    ui_handle_button(&state, BTN_DOWN, 2000);
    TEST_ASSERT_EQUAL(0, state.settings_cursor);
    ui_handle_button(&state, BTN_UP, 3000);
    TEST_ASSERT_EQUAL(1, state.settings_cursor);
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
    TEST_ASSERT_EQUAL(CONN_MODE_UI_COUNT - 1, state.settings_cursor);
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
    TEST_ASSERT_EQUAL(CONN_MODE_WIFI, conn_mode_resolve_boot(CONN_MODE_WIFI, true));
    TEST_ASSERT_EQUAL(CONN_MODE_WIFI, conn_mode_resolve_boot(CONN_MODE_WIFI, false));
    TEST_ASSERT_EQUAL(CONN_MODE_BLE, conn_mode_resolve_boot(CONN_MODE_BLE, true));
}

void test_conn_mode_resolve_boot_ble_unsupported_falls_back_to_wifi(void) {
    /* Persisted BLE mode on a stub-BLE build must boot WiFi, not a node
     * with no transport at all. */
    TEST_ASSERT_EQUAL(CONN_MODE_WIFI, conn_mode_resolve_boot(CONN_MODE_BLE, false));
}

void test_conn_mode_resolve_boot_off_valid_on_every_build(void) {
    /* Off needs no transport, so it must survive resolve on stub and
     * BLE-capable builds alike. */
    TEST_ASSERT_EQUAL(CONN_MODE_OFF, conn_mode_resolve_boot(CONN_MODE_OFF, true));
    TEST_ASSERT_EQUAL(CONN_MODE_OFF, conn_mode_resolve_boot(CONN_MODE_OFF, false));
}

void test_conn_mode_ui_index_round_trip(void) {
    /* The enum is sparse (legacy BOTH occupies 2, Off sits at 3), so the
     * UI index mapping is load-bearing; a direct cast regressed once. */
    conn_mode_t modes[] = {CONN_MODE_WIFI, CONN_MODE_BLE, CONN_MODE_OFF};
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(modes[i], conn_mode_from_ui_index(conn_mode_to_ui_index(modes[i])));
        TEST_ASSERT_EQUAL(i, conn_mode_to_ui_index(conn_mode_from_ui_index(i)));
    }
    TEST_ASSERT_EQUAL(0, conn_mode_to_ui_index(CONN_MODE_BOTH));
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
    TEST_ASSERT_EQUAL(0, state.unread_count);
    TEST_ASSERT_TRUE(state.message_auto_switch_time > 0);
}

void test_auto_switched_messages_survives_long_idle_tick(void) {
    /* A pager that has sat untouched far past every inactivity limit: the
     * common real-world state (a heltec on a desk for hours). */
    state.current_screen = SCREEN_MAIN;
    state.last_activity = 1000;

    ui_on_message_received(&state, 3600000); /* deeply idle: auto-switch fires */
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);

    /* The very next 50 ms main-loop tick runs the inactivity check. The
     * auto-switched view is governed by its own 30 s auto-restore; the general
     * inactivity revert must not bounce it back to MAIN before a human (or a
     * single render) can see it. This is exactly how "the screen never shows
     * messages" shipped: the switch happened and was reverted on the same
     * tick whenever the device had been idle longer than the Messages
     * inactivity limit. */
    ui_check_timeout(&state, 3600050);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);

    /* The timed restore still returns to the previous screen afterwards. */
    ui_check_timeout(&state, 3600000 + UI_MESSAGE_AUTO_RESTORE_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(SCREEN_MAIN, state.current_screen);
}

void test_incoming_message_while_active_increments_unread_without_switch(void) {
    state.current_screen = SCREEN_NODES;
    state.last_activity = 5000;

    ui_on_message_received(&state, 12000); /* 7s idle -> still active */
    ui_on_message_received(&state, 12500);

    TEST_ASSERT_EQUAL(SCREEN_NODES, state.current_screen);
    TEST_ASSERT_EQUAL(2, state.unread_count);
    TEST_ASSERT_EQUAL(0, state.message_auto_switch_time);
    TEST_ASSERT_TRUE(state.screen_dirty); /* badge must render */
}

void test_short_press_with_unread_jumps_to_messages_and_clears(void) {
    state.current_screen = SCREEN_NODES;
    state.last_activity = 5000;
    ui_on_message_received(&state, 12000);
    TEST_ASSERT_EQUAL(1, state.unread_count);

    ui_handle_button(&state, BTN_SHORT_PRESS, 12500);

    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);
    TEST_ASSERT_EQUAL(SCREEN_NODES, state.prev_screen);
    TEST_ASSERT_EQUAL(0, state.unread_count);
}

void test_cycling_into_messages_clears_unread(void) {
    state.current_screen = SCREEN_MAIN;
    state.last_activity = 5000;
    ui_on_message_received(&state, 12000);
    TEST_ASSERT_EQUAL(1, state.unread_count);

    /* Press-to-view fires from MAIN too; land on messages either way. */
    ui_handle_button(&state, BTN_SHORT_PRESS, 12500);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);
    TEST_ASSERT_EQUAL(0, state.unread_count);
}

void test_message_while_viewing_messages_does_not_count_unread(void) {
    state.current_screen = SCREEN_MESSAGES;
    state.last_activity = 5000;

    ui_on_message_received(&state, 12000);

    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);
    TEST_ASSERT_EQUAL(0, state.unread_count);
    TEST_ASSERT_TRUE(state.screen_dirty);
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

void test_long_press_on_messages_pages_older_and_clamps(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000); /* -> SCREEN_MESSAGES */
    ui_set_message_total(&state, 10);

    ui_handle_button(&state, BTN_LONG_PRESS, 2000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));
    TEST_ASSERT_EQUAL(4, state.msg_scroll);

    ui_handle_button(&state, BTN_LONG_PRESS, 3000);
    TEST_ASSERT_EQUAL(6, state.msg_scroll); /* clamp: 10 - 4 */

    ui_handle_button(&state, BTN_LONG_PRESS, 4000);
    TEST_ASSERT_EQUAL(6, state.msg_scroll); /* stays clamped */
}

void test_double_press_returns_to_newest_when_scrolled(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000); /* -> SCREEN_MESSAGES */
    ui_set_message_total(&state, 10);
    ui_handle_button(&state, BTN_LONG_PRESS, 2000);
    TEST_ASSERT_EQUAL(4, state.msg_scroll);

    ui_handle_button(&state, BTN_DOUBLE_PRESS, 3000);
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state)); /* no screen jump */
    TEST_ASSERT_EQUAL(0, state.msg_scroll);

    /* A second double-press (not scrolled) is the normal back-jump. */
    ui_handle_button(&state, BTN_DOUBLE_PRESS, 4000);
    TEST_ASSERT_NOT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));
}

void test_message_while_scrolled_counts_unread_and_blocks_restore(void) {
    state.current_screen = SCREEN_NODES;
    state.last_activity = 1000;
    ui_on_message_received(&state, 12050); /* idle -> auto-switch */
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);

    ui_set_message_total(&state, 10);
    ui_handle_button(&state, BTN_LONG_PRESS, 13000); /* scroll older */
    TEST_ASSERT_EQUAL(4, state.msg_scroll);

    /* New arrival while reading history: counted, no forced jump. */
    ui_on_message_received(&state, 13500);
    TEST_ASSERT_EQUAL(1, state.unread_count);
    TEST_ASSERT_EQUAL(4, state.msg_scroll);
}

void test_entering_messages_resets_scroll(void) {
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000); /* -> SCREEN_MESSAGES */
    ui_set_message_total(&state, 10);
    ui_handle_button(&state, BTN_LONG_PRESS, 2000);
    TEST_ASSERT_EQUAL(4, state.msg_scroll);

    /* Leave while scrolled, then jump back: history position must reset. */
    ui_handle_button(&state, BTN_SHORT_PRESS, 3000); /* -> SCREEN_NODES */
    TEST_ASSERT_EQUAL(SCREEN_NODES, ui_get_screen(&state));
    TEST_ASSERT_EQUAL(4, state.msg_scroll);

    ui_handle_button(&state, BTN_DOUBLE_PRESS, 3500); /* back to messages */
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, ui_get_screen(&state));
    TEST_ASSERT_EQUAL(0, state.msg_scroll);
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

void test_format_msg_line_incoming_named_sender(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "hello there",
                       .text_len = 11,
                       .outgoing = false,
                       .peer_addr = 0xA1B2C3D4,
                       .peer_name = "ally",
                       .channel_index = 0,
                       .channel_name = NULL,
                       .badge = "",
                       .age_s = -1};
    ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("ally: hello there", buf);
}

void test_format_msg_line_age_suffix(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "hello",
                       .text_len = 5,
                       .outgoing = false,
                       .peer_addr = 0xA1B2C3D4,
                       .peer_name = "ally",
                       .channel_index = 0,
                       .channel_name = NULL,
                       .badge = "",
                       .age_s = 300};
    ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("ally: hello 5m", buf);
}

void test_format_msg_line_unknown_sender_uses_hex(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "hi",
                       .text_len = 2,
                       .outgoing = false,
                       .peer_addr = 0xA1B2C3D4,
                       .peer_name = NULL,
                       .channel_index = 0,
                       .channel_name = NULL,
                       .badge = "",
                       .age_s = -1};
    ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("C3D4: hi", buf);
}

void test_format_msg_line_outgoing_with_badge_and_truncation(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "a very long message that will not fit",
                       .text_len = 38,
                       .outgoing = true,
                       .peer_addr = 0,
                       .peer_name = NULL,
                       .channel_index = 0,
                       .channel_name = NULL,
                       .badge = " +",
                       .age_s = -1};
    int n = ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n <= 21);
    /* used = "me" + ": " + " +" = 6, so 15 text chars fit on the 21-char line */
    TEST_ASSERT_EQUAL_STRING("me: a very long mes +", buf);
}

void test_format_msg_line_channel_tag(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "yo",
                       .text_len = 2,
                       .outgoing = false,
                       .peer_addr = 0xA1B2C3D4,
                       .peer_name = "ally",
                       .channel_index = 2,
                       .channel_name = "hiking",
                       .badge = "",
                       .age_s = -1};
    ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("#hiki ally: yo", buf);
}

void test_format_msg_line_channel_tag_falls_back_to_index(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "yo",
                       .text_len = 2,
                       .outgoing = false,
                       .peer_addr = 0xA1B2C3D4,
                       .peer_name = NULL,
                       .channel_index = 3,
                       .channel_name = "",
                       .badge = "",
                       .age_s = -1};
    ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("#3 C3D4: yo", buf);
}

void test_format_msg_line_action(void) {
    char buf[22];
    ui_msg_line_t m = {.text = "\x01"
                               "ACTION waves\x01",
                       .text_len = 14,
                       .outgoing = false,
                       .peer_addr = 0xA1B2C3D4,
                       .peer_name = "ally",
                       .channel_index = 0,
                       .channel_name = NULL,
                       .badge = "",
                       .age_s = -1};
    ui_format_msg_line(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("* ally waves", buf);
}

void test_nodes_selection_enter_cycle_open_detail(void) {
    state.current_screen = SCREEN_NODES;
    ui_set_node_total(&state, 3);

    ui_handle_button(&state, BTN_LONG_PRESS, 1000);
    TEST_ASSERT_TRUE(state.nodes_selecting);
    TEST_ASSERT_EQUAL(0, state.nodes_cursor);

    ui_handle_button(&state, BTN_SHORT_PRESS, 1100);
    TEST_ASSERT_EQUAL(1, state.nodes_cursor);
    ui_handle_button(&state, BTN_SHORT_PRESS, 1200);
    TEST_ASSERT_EQUAL(2, state.nodes_cursor);
    ui_handle_button(&state, BTN_SHORT_PRESS, 1300);
    TEST_ASSERT_EQUAL(0, state.nodes_cursor); /* wraps */

    ui_handle_button(&state, BTN_LONG_PRESS, 1400);
    TEST_ASSERT_TRUE(state.node_detail_open);
}

void test_nodes_detail_arm_then_confirm(void) {
    state.current_screen = SCREEN_NODES;
    ui_set_node_total(&state, 2);
    state.nodes_selecting = true;
    state.node_detail_open = true;

    ui_handle_button(&state, BTN_LONG_PRESS, 1000);
    TEST_ASSERT_TRUE(state.node_verify_armed);
    TEST_ASSERT_FALSE(state.node_verify_confirmed);

    ui_handle_button(&state, BTN_LONG_PRESS, 1100);
    TEST_ASSERT_TRUE(state.node_verify_confirmed);
    TEST_ASSERT_FALSE(state.node_verify_armed);
}

void test_nodes_detail_short_press_disarms_without_confirming(void) {
    state.current_screen = SCREEN_NODES;
    ui_set_node_total(&state, 2);
    state.nodes_selecting = true;
    state.node_detail_open = true;

    ui_handle_button(&state, BTN_LONG_PRESS, 1000);
    TEST_ASSERT_TRUE(state.node_verify_armed);

    ui_handle_button(&state, BTN_SHORT_PRESS, 1100);
    TEST_ASSERT_FALSE(state.node_verify_armed);
    TEST_ASSERT_FALSE(state.node_verify_confirmed);
}

void test_nodes_double_press_in_detail_returns_to_list(void) {
    state.current_screen = SCREEN_NODES;
    ui_set_node_total(&state, 2);
    state.nodes_selecting = true;
    state.node_detail_open = true;
    state.node_verify_armed = true;

    ui_handle_button(&state, BTN_DOUBLE_PRESS, 1000);
    TEST_ASSERT_FALSE(state.node_detail_open);
    TEST_ASSERT_FALSE(state.node_verify_armed);
    TEST_ASSERT_TRUE(state.nodes_selecting);
    TEST_ASSERT_EQUAL(SCREEN_NODES, ui_get_screen(&state));
}

void test_nodes_double_press_in_list_leaves_selection(void) {
    state.current_screen = SCREEN_NODES;
    ui_set_node_total(&state, 2);
    state.nodes_selecting = true;

    ui_handle_button(&state, BTN_DOUBLE_PRESS, 1000);
    TEST_ASSERT_FALSE(state.nodes_selecting);
    TEST_ASSERT_EQUAL(SCREEN_NODES, ui_get_screen(&state));
}

void test_nodes_entering_selection_with_zero_total_is_noop(void) {
    state.current_screen = SCREEN_NODES;
    ui_set_node_total(&state, 0);

    ui_handle_button(&state, BTN_LONG_PRESS, 1000);
    TEST_ASSERT_FALSE(state.nodes_selecting);
}

void test_nodes_selecting_reset_on_timeout_to_main(void) {
    state.current_screen = SCREEN_NODES;
    ui_set_node_total(&state, 3);
    state.nodes_selecting = true;
    state.node_detail_open = true;
    state.node_verify_armed = true;
    state.last_activity = 0;

    ui_check_timeout(&state, UI_INACTIVITY_TIMEOUT_MS + 1);
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
    TEST_ASSERT_FALSE(state.nodes_selecting);
    TEST_ASSERT_FALSE(state.node_detail_open);
    TEST_ASSERT_FALSE(state.node_verify_armed);
}

void test_nodes_short_press_while_not_selecting_still_cycles_screen(void) {
    state.current_screen = SCREEN_NODES;
    ui_set_node_total(&state, 3);

    ui_handle_button(&state, BTN_SHORT_PRESS, 1000);
    TEST_ASSERT_EQUAL(SCREEN_COMPOSE, ui_get_screen(&state));
    TEST_ASSERT_FALSE(state.nodes_selecting);
}

/* ── full-refresh policy (bramble#196) ───────────────────────────────── */

void test_full_refresh_not_pending_on_init(void) {
    /* Boot renders via the engine's own always-full first flush; the
     * screen-ring policy has nothing to clear yet. */
    TEST_ASSERT_FALSE(ui_take_full_refresh_pending(&state));
}

void test_full_refresh_pending_every_n_screen_changes(void) {
    /* MAIN -> MESSAGES -> NODES -> COMPOSE: the 3rd screen change (landing
     * on COMPOSE/"Stats") crosses UI_FULL_REFRESH_EVERY_N_SCREENS and must
     * request a full refresh, clearing ghosting from every screen visited
     * so far before Stats renders. */
    ui_handle_button(&state, BTN_SHORT_PRESS, 1000); /* -> MESSAGES: change #1 */
    TEST_ASSERT_FALSE(ui_take_full_refresh_pending(&state));

    ui_handle_button(&state, BTN_SHORT_PRESS, 2000); /* -> NODES: change #2 */
    TEST_ASSERT_FALSE(ui_take_full_refresh_pending(&state));

    ui_handle_button(&state, BTN_SHORT_PRESS, 3000); /* -> COMPOSE: change #3 */
    TEST_ASSERT_TRUE(ui_take_full_refresh_pending(&state));

    /* Consumed: asking again without another screen change returns false. */
    TEST_ASSERT_FALSE(ui_take_full_refresh_pending(&state));
}

void test_full_refresh_pending_on_entering_settings(void) {
    /* Settings is the screen bramble#196 called out by name (most
     * text-dense, so the worst-hit by ghosting): entering it forces a full
     * refresh unconditionally, even on the very first screen change. */
    ui_handle_button(&state, BTN_LEFT, 1000); /* MAIN -> SETTINGS (previous) */
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));
    TEST_ASSERT_TRUE(ui_take_full_refresh_pending(&state));
}

void test_full_refresh_not_pending_on_in_screen_redraw(void) {
    /* Settings row navigation and edit-mode changes redraw without leaving
     * SCREEN_SETTINGS: these must not consume the screen-change cadence or
     * spuriously request a full refresh (the ghosting they can add is a
     * cursor moving inside already-full-refreshed content, not new pages'
     * worth of stale ink). */
    ui_handle_button(&state, BTN_LEFT, 1000); /* MAIN -> SETTINGS, consumes the pending flag */
    ui_take_full_refresh_pending(&state);

    ui_handle_button(&state, BTN_DOWN, 2000); /* row nav within Settings */
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));
    TEST_ASSERT_FALSE(ui_take_full_refresh_pending(&state));

    ui_handle_button(&state, BTN_SELECT, 3000); /* enter edit mode, still Settings */
    TEST_ASSERT_EQUAL(SCREEN_SETTINGS, ui_get_screen(&state));
    TEST_ASSERT_FALSE(ui_take_full_refresh_pending(&state));
}

void test_full_refresh_pending_survives_screen_leaving_settings(void) {
    /* Leaving Settings is a screen change like any other: it counts toward
     * the every-N cadence but does not itself force a refresh (only
     * entering Settings, or hitting the N-count, does). */
    state.current_screen = SCREEN_SETTINGS;
    state.settings_editing = false;
    ui_take_full_refresh_pending(&state); /* clear any incidental state */

    ui_handle_button(&state, BTN_DOUBLE_PRESS, 1000); /* Settings -> MAIN */
    TEST_ASSERT_EQUAL(SCREEN_MAIN, ui_get_screen(&state));
    TEST_ASSERT_FALSE(ui_take_full_refresh_pending(&state));
}

void test_full_refresh_pending_on_idle_auto_switch_to_messages(void) {
    /* The idle auto-switch to Messages goes through ui_on_message_received,
     * not ui_handle_button; it must feed the same screen-change cadence. */
    state.current_screen = SCREEN_NODES;
    state.last_activity = 1000;
    state.screens_since_full_refresh = UI_FULL_REFRESH_EVERY_N_SCREENS - 1;

    ui_on_message_received(&state, 12050); /* idle -> auto-switch to MESSAGES */
    TEST_ASSERT_EQUAL(SCREEN_MESSAGES, state.current_screen);
    TEST_ASSERT_TRUE(ui_take_full_refresh_pending(&state));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_main_screen);
    RUN_TEST(test_short_press_cycles);
    RUN_TEST(test_double_press_back);
    RUN_TEST(test_inactivity_timeout);
    RUN_TEST(test_messages_screen_gets_long_inactivity_timeout);
    RUN_TEST(test_screen_dirty_on_transition);
    RUN_TEST(test_long_press_dirty_no_change);
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
    RUN_TEST(test_gps_screen_skipped_by_default);
    RUN_TEST(test_gps_screen_reachable_when_available);
    RUN_TEST(test_gps_screen_skipped_going_backward_when_unavailable);
    RUN_TEST(test_gps_screen_reachable_going_backward_when_available);
    RUN_TEST(test_trackball_select_on_messages_opens_compose);
    RUN_TEST(test_conn_mode_resolve_boot_keeps_supported_modes);
    RUN_TEST(test_conn_mode_resolve_boot_ble_unsupported_falls_back_to_wifi);
    RUN_TEST(test_conn_mode_resolve_boot_normalizes_legacy_both);
    RUN_TEST(test_conn_mode_resolve_boot_off_valid_on_every_build);
    RUN_TEST(test_conn_mode_ui_index_round_trip);
    RUN_TEST(test_trackball_settings_row_navigation_when_not_editing);
    RUN_TEST(test_settings_gps_row_skipped_when_unavailable);
    RUN_TEST(test_settings_gps_row_reachable_when_available);
    RUN_TEST(test_settings_gps_edit_toggles_off_and_on);
    RUN_TEST(test_trackball_select_on_settings_enters_edit_for_selected_row);
    RUN_TEST(test_trackball_settings_edit_navigation_connectivity_row);
    RUN_TEST(test_trackball_settings_edit_navigation_oled_row);
    RUN_TEST(test_trackball_settings_edit_confirm_with_select);
    RUN_TEST(test_trackball_settings_edit_cancel_with_left);
    RUN_TEST(test_location_ui_actions_toggle_tier_interval);
    RUN_TEST(test_location_ui_panic_off_disables_sharing);
    RUN_TEST(test_location_ui_status_indicators);
    RUN_TEST(test_incoming_message_idle_auto_switches_to_messages);
    RUN_TEST(test_incoming_message_while_active_increments_unread_without_switch);
    RUN_TEST(test_short_press_with_unread_jumps_to_messages_and_clears);
    RUN_TEST(test_cycling_into_messages_clears_unread);
    RUN_TEST(test_message_while_viewing_messages_does_not_count_unread);
    RUN_TEST(test_long_press_on_messages_pages_older_and_clamps);
    RUN_TEST(test_double_press_returns_to_newest_when_scrolled);
    RUN_TEST(test_message_while_scrolled_counts_unread_and_blocks_restore);
    RUN_TEST(test_entering_messages_resets_scroll);
    RUN_TEST(test_format_msg_line_incoming_named_sender);
    RUN_TEST(test_format_msg_line_age_suffix);
    RUN_TEST(test_format_msg_line_unknown_sender_uses_hex);
    RUN_TEST(test_format_msg_line_outgoing_with_badge_and_truncation);
    RUN_TEST(test_format_msg_line_channel_tag);
    RUN_TEST(test_format_msg_line_channel_tag_falls_back_to_index);
    RUN_TEST(test_format_msg_line_action);
    RUN_TEST(test_auto_restore_returns_to_previous_screen_after_timeout);
    RUN_TEST(test_user_interaction_on_messages_cancels_auto_restore);
    RUN_TEST(test_nodes_selection_enter_cycle_open_detail);
    RUN_TEST(test_nodes_detail_arm_then_confirm);
    RUN_TEST(test_nodes_detail_short_press_disarms_without_confirming);
    RUN_TEST(test_nodes_double_press_in_detail_returns_to_list);
    RUN_TEST(test_nodes_double_press_in_list_leaves_selection);
    RUN_TEST(test_nodes_entering_selection_with_zero_total_is_noop);
    RUN_TEST(test_nodes_selecting_reset_on_timeout_to_main);
    RUN_TEST(test_nodes_short_press_while_not_selecting_still_cycles_screen);
    RUN_TEST(test_auto_switched_messages_survives_long_idle_tick);
    RUN_TEST(test_full_refresh_not_pending_on_init);
    RUN_TEST(test_full_refresh_pending_every_n_screen_changes);
    RUN_TEST(test_full_refresh_pending_on_entering_settings);
    RUN_TEST(test_full_refresh_not_pending_on_in_screen_redraw);
    RUN_TEST(test_full_refresh_pending_survives_screen_leaving_settings);
    RUN_TEST(test_full_refresh_pending_on_idle_auto_switch_to_messages);
    return UNITY_END();
}
