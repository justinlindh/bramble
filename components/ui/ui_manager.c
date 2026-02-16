#include "ui.h"
#include <string.h>

void ui_init(ui_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->current_screen = SCREEN_MAIN;
    state->prev_screen = SCREEN_MAIN;
    state->screen_dirty = true;
}

void ui_handle_button(ui_state_t *state, ui_button_t btn, uint32_t now_ms) {
    state->last_activity = now_ms;

    switch (btn) {
    case BTN_SHORT_PRESS: {
        ui_screen_t prev = state->current_screen;
        state->prev_screen = prev;
        state->current_screen = (ui_screen_t)((prev + 1) % SCREEN_COUNT);
        state->screen_enter_time = now_ms;
        state->screen_dirty = true;
        break;
    }
    case BTN_LONG_PRESS:
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
