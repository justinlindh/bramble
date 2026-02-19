/**
 * Keyboard driver for T-Deck Plus ESP32-C3 sub-MCU
 * I2C keyboard interface at address 0x55
 */

#ifndef BRAMBLE_KEYBOARD_H
#define BRAMBLE_KEYBOARD_H

#include <stdbool.h>

/**
 * Initialize keyboard I2C and interrupt.
 * Returns 0 on success, -1 on failure.
 */
int keyboard_init(void);

/**
 * Poll for a keypress.
 * Returns true and writes the ASCII character to *out if a key is available.
 * Returns false if no key is pending.
 */
bool keyboard_poll(char *out);

/**
 * Check if keyboard has data pending.
 * Returns true if at least one key is buffered.
 */
bool keyboard_has_data(void);

#endif /* BRAMBLE_KEYBOARD_H */
