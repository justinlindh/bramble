/**
 * Keyboard driver for T-Deck Plus ESP32-C3 sub-MCU
 * I2C keyboard interface at address 0x55
 */

#ifndef BRAMBLE_KEYBOARD_H
#define BRAMBLE_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize keyboard I2C and interrupt.
 * Returns 0 on success, -1 on failure.
 */
int keyboard_init(void);

/**
 * Get the I2C master bus handle (shared with touch, sensors).
 * Returns NULL if keyboard not initialized.
 */
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#include "driver/i2c_master.h"
i2c_master_bus_handle_t keyboard_get_i2c_bus(void);
#endif

/**
 * Poll for a keypress.
 * Returns true and writes the ASCII character to *out if a key is available.
 * Returns false if no key is pending.
 */
bool keyboard_poll(char* out);

/**
 * Check if keyboard has data pending.
 * Returns true if at least one key is buffered.
 */
bool keyboard_has_data(void);

/**
 * Set keyboard backlight brightness via I2C command to keyboard MCU.
 * brightness: 0 = off, 255 = maximum.  Values are passed directly to the
 * MCU as a PWM duty cycle; if the MCU firmware treats it as on/off only,
 * any value >0 will enable the backlight.
 */
void keyboard_set_backlight(uint8_t brightness);

/**
 * Set keyboard backlight brightness as percentage (0-100) and persist to NVS.
 * percent: 0 = off, 100 = maximum.  Automatically converts to 0-255 range
 * for hardware and saves to NVS for persistence across reboots.
 */
void keyboard_set_backlight_percent(uint8_t percent);

/**
 * Get the current keyboard backlight brightness as percentage (0-100).
 * Returns the persisted value from NVS.
 */
uint8_t keyboard_get_backlight_percent(void);

#endif /* BRAMBLE_KEYBOARD_H */
