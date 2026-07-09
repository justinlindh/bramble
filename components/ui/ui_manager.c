#include "ui.h"
#include "location.h"
#include <string.h>

void ui_init(ui_state_t* state) {
    memset(state, 0, sizeof(*state));
    state->current_screen = SCREEN_MAIN;
    state->prev_screen = SCREEN_MAIN;
    state->screen_dirty = true;
}

void ui_handle_button(ui_state_t* state, ui_button_t btn, uint32_t now_ms) {
    state->last_activity = now_ms;

    if (state->current_screen == SCREEN_MESSAGES && state->message_auto_switch_time != 0) {
        /* User took control while in auto-switched messages view; cancel timed restore. */
        state->message_auto_switch_time = 0;
    }
    /* Settings screen editing */
    if (state->current_screen == SCREEN_SETTINGS && state->settings_editing) {
        int value_count;
        switch (state->settings_item_cursor) {
        case UI_SETTINGS_ITEM_CONN_MODE:
            value_count = CONN_MODE_COUNT;
            break;
        case UI_SETTINGS_ITEM_LOCATION:
            value_count = LOC_SHARE_COUNT;
            break;
        default:
            value_count = 2;
            break; /* OLED rotation */
        }
        switch (btn) {
        case BTN_SHORT_PRESS:
        case BTN_UP: /* trackball up = previous option */
            state->settings_cursor = (state->settings_cursor + value_count - 1) % value_count;
            state->screen_dirty = true;
            break;
        case BTN_DOWN: /* trackball down = next option */
            state->settings_cursor = (state->settings_cursor + 1) % value_count;
            state->screen_dirty = true;
            break;
        case BTN_LONG_PRESS:
        case BTN_SELECT: /* trackball center = confirm */
            state->settings_confirmed = true;
            state->screen_dirty = true;
            break;
        case BTN_DOUBLE_PRESS:
        case BTN_LEFT: /* trackball left = cancel/back */
            state->settings_editing = false;
            state->screen_dirty = true;
            break;
        default:
            break;
        }
        return;
    }

    /* Settings screen row navigation (non-edit mode) */
    if (state->current_screen == SCREEN_SETTINGS && !state->settings_editing) {
        switch (btn) {
        case BTN_SHORT_PRESS: /* single-button: cycle through settings items */
        case BTN_DOWN:
            state->settings_item_cursor =
                (ui_settings_item_t)((state->settings_item_cursor + 1) % UI_SETTINGS_ITEM_COUNT);
            state->screen_dirty = true;
            return;
        case BTN_UP:
            state->settings_item_cursor =
                (ui_settings_item_t)((state->settings_item_cursor + UI_SETTINGS_ITEM_COUNT - 1) %
                                     UI_SETTINGS_ITEM_COUNT);
            state->screen_dirty = true;
            return;
        case BTN_LONG_PRESS:
        case BTN_SELECT:
            state->settings_editing = true;
            state->screen_dirty = true;
            return;
        case BTN_DOUBLE_PRESS: /* double-press exits settings screen */
            state->prev_screen = state->current_screen;
            state->current_screen = SCREEN_MAIN;
            state->screen_dirty = true;
            return;
        default:
            break;
        }
    }

    /* Messages screen: long-press pages into history, double-press
     * returns to the newest page (instead of jumping screens). */
    if (state->current_screen == SCREEN_MESSAGES) {
        if (btn == BTN_LONG_PRESS) {
            int max_scroll =
                (state->msg_total > UI_MSG_PAGE_LINES) ? state->msg_total - UI_MSG_PAGE_LINES : 0;
            int next = state->msg_scroll + UI_MSG_PAGE_LINES;
            state->msg_scroll = (next > max_scroll) ? max_scroll : next;
            state->screen_dirty = true;
            return;
        }
        if (btn == BTN_DOUBLE_PRESS && state->msg_scroll > 0) {
            state->msg_scroll = 0;
            state->unread_count = 0; /* newest page is now visible */
            state->screen_dirty = true;
            return;
        }
    }

    switch (btn) {
    case BTN_SHORT_PRESS:
    case BTN_RIGHT: /* trackball right = next screen */
    case BTN_DOWN:  /* trackball down = next screen */
    {
        /* Press-to-view: while anything is unread, a short press jumps
         * straight to the messages screen instead of cycling. */
        if (btn == BTN_SHORT_PRESS && state->unread_count > 0 &&
            state->current_screen != SCREEN_MESSAGES) {
            state->prev_screen = state->current_screen;
            state->current_screen = SCREEN_MESSAGES;
            state->screen_enter_time = now_ms;
            state->screen_dirty = true;
            break;
        }
        ui_screen_t prev = state->current_screen;
        state->prev_screen = prev;
        ui_screen_t next = (ui_screen_t)((prev + 1) % SCREEN_COUNT);
        if (next == SCREEN_GPS && !state->gps_available) {
            next = (ui_screen_t)((next + 1) % SCREEN_COUNT);
        }
        state->current_screen = next;
        state->screen_enter_time = now_ms;
        state->screen_dirty = true;
        break;
    }
    case BTN_LEFT: /* trackball left = previous screen */
    case BTN_UP:   /* trackball up = previous screen */
    {
        ui_screen_t prev = state->current_screen;
        state->prev_screen = prev;
        ui_screen_t next = (ui_screen_t)((prev + SCREEN_COUNT - 1) % SCREEN_COUNT);
        if (next == SCREEN_GPS && !state->gps_available) {
            next = (ui_screen_t)((next + SCREEN_COUNT - 1) % SCREEN_COUNT);
        }
        state->current_screen = next;
        state->screen_enter_time = now_ms;
        state->screen_dirty = true;
        break;
    }
    case BTN_SELECT: /* trackball center = same as short press for now */
    {
        /* On messages screen: could open compose. On settings: enter edit mode */
        if (state->current_screen == SCREEN_SETTINGS) {
            state->settings_editing = true;
            state->screen_dirty = true;
        } else if (state->current_screen == SCREEN_MESSAGES) {
            /* Enter compose mode */
            state->prev_screen = state->current_screen;
            state->current_screen = SCREEN_COMPOSE;
            state->screen_enter_time = now_ms;
            state->compose_len = 0;
            state->compose_buf[0] = '\0';
            state->compose_active = true;
            state->screen_dirty = true;
        }
        break;
    }
    case BTN_LONG_PRESS:
        if (state->current_screen == SCREEN_SETTINGS) {
            state->settings_editing = true;
        }
        state->screen_dirty = true;
        break;
    case BTN_DOUBLE_PRESS: {
        ui_screen_t cur = state->current_screen;
        state->current_screen = state->prev_screen;
        state->prev_screen = cur;
        state->screen_enter_time = now_ms;
        state->screen_dirty = true;
        break;
    }
    default:
        break;
    }

    if (state->current_screen == SCREEN_MESSAGES) {
        state->unread_count = 0;
        if (state->prev_screen != SCREEN_MESSAGES) {
            state->msg_scroll = 0;
        }
    }
}

void ui_set_gps_available(ui_state_t* state, bool available) { state->gps_available = available; }

void ui_set_message_total(ui_state_t* state, int total) { state->msg_total = total; }

ui_screen_t ui_get_screen(const ui_state_t* state) { return state->current_screen; }

bool ui_needs_redraw(const ui_state_t* state) { return state->screen_dirty; }

void ui_mark_drawn(ui_state_t* state) { state->screen_dirty = false; }

void ui_on_message_received(ui_state_t* state, uint32_t now_ms) {
    if (state->current_screen == SCREEN_MESSAGES && state->msg_scroll == 0) {
        /* Reader is already looking at the newest page: nothing pending, but
         * extend the auto-restore window if one is running and repaint. */
        if (state->message_auto_switch_time != 0) {
            state->message_auto_switch_time = now_ms;
        }
        state->screen_dirty = true;
        return;
    }
    if (state->current_screen == SCREEN_MESSAGES) {
        /* Scrolled into history: count it, do not yank the view. */
        if (state->unread_count < 99) {
            state->unread_count++;
        }
        state->screen_dirty = true;
        return;
    }

    uint32_t idle_ms = now_ms - state->last_activity;
    if (idle_ms >= UI_MESSAGE_IDLE_THRESHOLD_MS) {
        state->prev_screen = state->current_screen;
        state->current_screen = SCREEN_MESSAGES;
        state->screen_enter_time = now_ms;
        state->unread_count = 0;
        state->msg_scroll = 0;
        state->message_auto_switch_time = now_ms;
        state->screen_dirty = true;
    } else {
        if (state->unread_count < 99) {
            state->unread_count++;
        }
        state->screen_dirty = true;
    }
}

void ui_check_timeout(ui_state_t* state, uint32_t now_ms) {
    if (state->message_auto_switch_time != 0 && state->current_screen == SCREEN_MESSAGES &&
        state->msg_scroll == 0 &&
        (now_ms - state->message_auto_switch_time) >= UI_MESSAGE_AUTO_RESTORE_TIMEOUT_MS) {
        state->current_screen = state->prev_screen;
        state->screen_enter_time = now_ms;
        state->screen_dirty = true;
        state->message_auto_switch_time = 0;
        return;
    }

    uint32_t inactivity_limit = (state->current_screen == SCREEN_MESSAGES)
                                    ? UI_MESSAGES_INACTIVITY_TIMEOUT_MS
                                    : UI_INACTIVITY_TIMEOUT_MS;
    if (state->current_screen != SCREEN_MAIN &&
        (now_ms - state->last_activity) >= inactivity_limit) {
        state->prev_screen = state->current_screen;
        state->current_screen = SCREEN_MAIN;
        state->screen_enter_time = now_ms;
        state->screen_dirty = true;
        state->last_activity = now_ms;
        state->message_auto_switch_time = 0;
    }
}

conn_mode_t conn_mode_resolve_boot(conn_mode_t requested, bool low_sram_board) {
    (void)low_sram_board;
    if (requested == CONN_MODE_BOTH) {
        return CONN_MODE_WIFI;
    }
    return requested;
}
