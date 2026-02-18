#ifndef BRAMBLE_BATTERY_H
#define BRAMBLE_BATTERY_H

#include <stdint.h>

/**
 * Initialize battery ADC reading.
 * Heltec V3: GPIO1 with voltage divider (factor ~2).
 */
void battery_init(void);

/**
 * Read battery voltage in millivolts.
 * Returns 0 if not initialized or read fails.
 */
uint32_t battery_read_mv(void);

/**
 * Convert millivolts to percentage (0-100).
 * Uses a LiPo discharge curve approximation.
 */
uint8_t battery_mv_to_pct(uint32_t mv);

/**
 * Convenience: read + convert in one call.
 */
uint8_t battery_read_pct(void);

#endif
