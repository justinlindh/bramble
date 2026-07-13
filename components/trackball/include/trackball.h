/**
 * Trackball driver for T-Deck Plus
 * Hall effect sensor trackball with 5 inputs (up, down, left, right, center)
 */

#ifndef BRAMBLE_TRACKBALL_H
#define BRAMBLE_TRACKBALL_H

#include "ui.h" /* For ui_button_t */

/**
 * Initialize trackball GPIOs and interrupts.
 * Returns 0 on success, -1 on failure.
 */
int trackball_init(void);

/**
 * Poll for trackball events.
 * Returns BTN_NONE if no event is pending, or one of:
 *   BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_SELECT
 * Priority: center > up > down > left > right
 */
ui_button_t trackball_poll(void);

/**
 * Inject a synthetic trackball event (bench debug RPC, bramble.injectInput).
 * Feeds the same event counters the GPIO ISRs increment, so trackball_poll()
 * drains it through the identical priority logic as a real hall-effect
 * detent. btn must be one of BTN_UP/DOWN/LEFT/RIGHT/SELECT.
 * Returns true if queued, false if not initialized or btn is invalid.
 */
bool trackball_inject(ui_button_t btn);

#endif /* BRAMBLE_TRACKBALL_H */
