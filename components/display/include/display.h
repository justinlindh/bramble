#ifndef BRAMBLE_DISPLAY_H
#define BRAMBLE_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Display dimensions: board-specific */
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 240
#elif defined(CONFIG_BRAMBLE_BOARD_PAGER) || defined(CONFIG_BRAMBLE_BOARD_VIRTUAL_PAGER)
/* Bramble Pager: GDEY0213B74 SSD1680 e-paper, landscape */
#define DISPLAY_WIDTH 250
#define DISPLAY_HEIGHT 122
#else
/* Default: Heltec V3 and other SSD1306 boards */
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#endif

/* I2C parameters (for SSD1306 boards) */
#define DISPLAY_I2C_PORT 0
#define DISPLAY_I2C_FREQ_HZ 400000

/**
 * Initialize the SSD1306 OLED display over I2C.
 * Resets via RST pin, configures I2C, sends init sequence.
 * Returns 0 on success, -1 on failure.
 */
int display_init(void);

/** Clear the framebuffer (all pixels off). */
void display_clear(void);

/**
 * Draw a string at pixel position (x, y).
 * Uses built-in 6x8 font. Clips at display boundaries.
 */
void display_draw_text(int x, int y, const char* text);

/**
 * Draw a string with 2x scaling (12x16 effective).
 * Used for large text like the splash screen title.
 */
void display_draw_text_large(int x, int y, const char* text);

/** Draw a horizontal line from (x, y) to (x+w, y). */
void display_hline(int x, int y, int w);

/** Set/clear a single pixel. */
void display_pixel(int x, int y, bool on);

/** Flush the framebuffer to the display via I2C. */
void display_flush(void);

/**
 * Request that the next display_flush() perform a full (flashing) refresh
 * instead of a partial one. On the pager's SSD1680 e-paper panel this
 * clears residue accumulated from prior partial refreshes; on OLED/LCD
 * boards there is no such residue and this is a no-op.
 */
void display_request_full_refresh(void);

/** Turn the display on or off (power save). */
void display_power(bool on);

/** Set backlight brightness via LEDC PWM (0=off, 255=full). */
void display_set_backlight(uint8_t level);

/** Last level passed to display_set_backlight (255 before any call). */
uint8_t display_get_backlight(void);

/**
 * Rotate display output by 180 degrees at runtime.
 * For SSD1306 this updates controller segment/COM scan mapping.
 */
void display_set_rotated_180(bool rotated);

/** Return whether display is currently configured for 180-degree rotation. */
bool display_get_rotated_180(void);

/**
 * Flush a rectangular area of pixels to the display.
 * buf contains RGB565 pixels in row-major order.
 * Used by LVGL display driver.
 */
void display_flush_area(int x1, int y1, int x2, int y2, const uint16_t* buf);

#endif
