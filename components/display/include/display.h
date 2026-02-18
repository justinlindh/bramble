#ifndef BRAMBLE_DISPLAY_H
#define BRAMBLE_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

/* Heltec WiFi LoRa 32 V3 OLED pins */
#define DISPLAY_SDA_PIN     17
#define DISPLAY_SCL_PIN     18
#define DISPLAY_RST_PIN     21
#define DISPLAY_VEXT_PIN    36  /* Power gate: LOW = OLED on */

/* SSD1306 128x64 parameters */
#define DISPLAY_WIDTH       128
#define DISPLAY_HEIGHT      64
#define DISPLAY_I2C_ADDR    0x3C
#define DISPLAY_I2C_PORT    0
#define DISPLAY_I2C_FREQ_HZ 400000

/**
 * Initialize the SSD1306 OLED display over I2C.
 * Resets via RST pin, configures I2C, sends init sequence.
 * Returns 0 on success, -1 on failure.
 */
int display_init(void);

/** Clear the framebuffer (all pixels off). */
void display_clear(void);

/** Fill the entire display (all pixels on). */
void display_fill(void);

/**
 * Draw a string at pixel position (x, y).
 * Uses built-in 6x8 font. Clips at display boundaries.
 */
void display_draw_text(int x, int y, const char *text);

/**
 * Draw a string with 2x scaling (12x16 effective).
 * Used for large text like the splash screen title.
 */
void display_draw_text_large(int x, int y, const char *text);

/** Draw a horizontal line from (x, y) to (x+w, y). */
void display_hline(int x, int y, int w);

/** Set/clear a single pixel. */
void display_pixel(int x, int y, bool on);

/** Flush the framebuffer to the display via I2C. */
void display_flush(void);

/** Turn the display on or off (power save). */
void display_power(bool on);

/** Set display contrast (0-255). */
void display_set_contrast(uint8_t val);

/** Invert display colors. */
void display_invert(bool invert);

#endif
