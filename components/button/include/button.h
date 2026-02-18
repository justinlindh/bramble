#ifndef BRAMBLE_BUTTON_H
#define BRAMBLE_BUTTON_H

#include <stdint.h>
#include "ui.h"

/* Heltec V3 PRG button = GPIO0 (active low) */
#define BUTTON_GPIO     0

/* Timing thresholds (ms) */
#define BUTTON_DEBOUNCE_MS      50
#define BUTTON_LONG_PRESS_MS    800
#define BUTTON_DOUBLE_GAP_MS    300

/**
 * Initialize button GPIO with internal pull-up and interrupt.
 */
void button_init(void);

/**
 * Poll for button events. Call from your main loop.
 * Returns the detected event (BTN_NONE if nothing happened).
 */
ui_button_t button_poll(uint32_t now_ms);

#endif
