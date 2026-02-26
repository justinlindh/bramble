#include "ui.h"
#include "location.h"
#include <string.h>

void ui_init(ui_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->current_screen = SCREEN_MAIN;
    state->prev_screen = SCREEN_MAIN;
    state->screen_dirty = true;
}

void ui_handle_button(ui_state_t *state, ui_button_t btn, uint32_t now_ms) {
    state->last_activity = now_ms;

    /* Settings screen editing */
    if (state->current_screen == SCREEN_SETTINGS && state->settings_editing) {
        int value_count;
        switch (state->settings_item_cursor) {
        case UI_SETTINGS_ITEM_CONN_MODE: value_count = CONN_MODE_COUNT; break;
        case UI_SETTINGS_ITEM_LOCATION:  value_count = LOC_SHARE_COUNT; break;
        default:                         value_count = 2; break; /* OLED rotation */
        }
        switch (btn) {
        case BTN_SHORT_PRESS:
        case BTN_UP:    /* trackball up = previous option */
            state->settings_cursor = (state->settings_cursor + value_count - 1) % value_count;
            state->screen_dirty = true;
            break;
        case BTN_DOWN:  /* trackball down = next option */
            state->settings_cursor = (state->settings_cursor + 1) % value_count;
            state->screen_dirty = true;
            break;
        case BTN_LONG_PRESS:
        case BTN_SELECT: /* trackball center = confirm */
            state->settings_confirmed = true;
            state->screen_dirty = true;
            break;
        case BTN_DOUBLE_PRESS:
        case BTN_LEFT:  /* trackball left = cancel/back */
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
        case BTN_UP:
            state->settings_item_cursor = (ui_settings_item_t)((state->settings_item_cursor + UI_SETTINGS_ITEM_COUNT - 1) % UI_SETTINGS_ITEM_COUNT);
            state->screen_dirty = true;
            return;
        case BTN_DOWN:
            state->settings_item_cursor = (ui_settings_item_t)((state->settings_item_cursor + 1) % UI_SETTINGS_ITEM_COUNT);
            state->screen_dirty = true;
            return;
        case BTN_LONG_PRESS:
        case BTN_SELECT:
            state->settings_editing = true;
            state->screen_dirty = true;
            return;
        default:
            break;
        }
    }

    switch (btn) {
    case BTN_SHORT_PRESS:
    case BTN_RIGHT:     /* trackball right = next screen */
    case BTN_DOWN:      /* trackball down = next screen */
    {
        ui_screen_t prev = state->current_screen;
        state->prev_screen = prev;
        state->current_screen = (ui_screen_t)((prev + 1) % SCREEN_COUNT);
        state->screen_enter_time = now_ms;
        state->screen_dirty = true;
        break;
    }
    case BTN_LEFT:      /* trackball left = previous screen */
    case BTN_UP:        /* trackball up = previous screen */
    {
        ui_screen_t prev = state->current_screen;
        state->prev_screen = prev;
        state->current_screen = (ui_screen_t)((prev + SCREEN_COUNT - 1) % SCREEN_COUNT);
        state->screen_enter_time = now_ms;
        state->screen_dirty = true;
        break;
    }
    case BTN_SELECT:    /* trackball center = same as short press for now */
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
}

ui_screen_t ui_get_screen(const ui_state_t *state) {
    return state->current_screen;
}

bool ui_needs_redraw(const ui_state_t *state) {
    return state->screen_dirty;
}

void ui_mark_drawn(ui_state_t *state) {
    state->screen_dirty = false;
}

void ui_check_timeout(ui_state_t *state, uint32_t now_ms) {
    if (state->current_screen != SCREEN_MAIN &&
        (now_ms - state->last_activity) >= UI_INACTIVITY_TIMEOUT_MS) {
        state->prev_screen = state->current_screen;
        state->current_screen = SCREEN_MAIN;
        state->screen_enter_time = now_ms;
        state->screen_dirty = true;
        state->last_activity = now_ms;
    }
}

conn_mode_t conn_mode_resolve_boot(conn_mode_t requested, bool low_sram_board) {
    (void)low_sram_board;
    if (requested == CONN_MODE_BOTH) {
        return CONN_MODE_WIFI;
    }
    return requested;
}
