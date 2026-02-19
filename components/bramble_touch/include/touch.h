#ifndef BRAMBLE_TOUCH_H
#define BRAMBLE_TOUCH_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int16_t x;
    int16_t y;
    bool pressed;
} touch_point_t;

/**
 * Initialize the GT911 touch controller.
 * Uses I2C bus already initialized by keyboard driver.
 * Returns 0 on success, -1 on failure.
 */
int touch_init(void);

/**
 * Read current touch state.
 * Returns true if a touch point was read, false if no touch or error.
 */
bool touch_read(touch_point_t *point);

#endif
