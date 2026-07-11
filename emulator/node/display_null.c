/*
 * Spike-only no-op display driver for the IDF linux target.
 *
 * Implements components/display/include/display.h function-for-function as
 * no-ops; only the trivial state getters (backlight, rotation, dimensions)
 * behave. Replaced by the ssd1680_engine + display_virt pipeline in a later
 * task.
 */
#include "display.h"

static uint8_t s_backlight = 255;
static bool s_rotated_180 = false;

int display_init(void) { return 0; }

void display_clear(void) {}

void display_fill(void) {}

void display_draw_text(int x, int y, const char* text) {
    (void)x;
    (void)y;
    (void)text;
}

void display_draw_text_large(int x, int y, const char* text) {
    (void)x;
    (void)y;
    (void)text;
}

void display_hline(int x, int y, int w) {
    (void)x;
    (void)y;
    (void)w;
}

void display_pixel(int x, int y, bool on) {
    (void)x;
    (void)y;
    (void)on;
}

void display_flush(void) {}

void display_power(bool on) { (void)on; }

void display_set_backlight(uint8_t level) { s_backlight = level; }

uint8_t display_get_backlight(void) { return s_backlight; }

void display_set_contrast(uint8_t val) { (void)val; }

void display_invert(bool invert) { (void)invert; }

void display_set_rotated_180(bool rotated) { s_rotated_180 = rotated; }

bool display_get_rotated_180(void) { return s_rotated_180; }

void display_flush_area(int x1, int y1, int x2, int y2, const uint16_t* buf) {
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)buf;
}

int display_get_width(void) { return DISPLAY_WIDTH; }

int display_get_height(void) { return DISPLAY_HEIGHT; }
