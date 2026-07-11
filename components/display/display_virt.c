/*
 * Virtual e-paper backend for the emulator (IDF linux target).
 *
 * Implements display.h on top of the pure SSD1680 engine, exactly like
 * ssd1680_io.c does on the device, but instead of replaying the command
 * stream over SPI it ships the resolved logical framebuffer to the
 * emu-link broker as an "fb" message (emulator/DESIGN.md section 8):
 *   { t:"fb", seq, kind:"partial"|"full", fb:<base64 3904 B>, busy_ms }
 * The frontend, not the firmware, renders e-paper physics; the engine
 * still decides refresh kind and busy duration so the emulated panel
 * behaves like the real one will.
 *
 * fb payload layout: see ssd1680_engine.h (250x122 1bpp row-major,
 * 32 bytes per row, MSB = leftmost pixel, bit set = black ink, 6 pad
 * bits in the LSBs of each row's last byte).
 */
#include "display.h"
#include "font_6x8.h"
#include "ssd1680_engine.h"
#include "emu_link.h"
#include "cJSON.h"

#include <stdint.h>

static bool s_initialized = false;
static bool s_rotated_180 = false;
static uint32_t s_seq = 0; /* monotonically increasing, first frame = 1 */

/* ── base64 (standard alphabet, padded) ──────────────────────────────── */

static const char b64_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#define FB_B64_LEN (((SSD1680_FB_SIZE + 2) / 3) * 4)
static char s_b64[FB_B64_LEN + 1];

static void fb_to_b64(const uint8_t* in) {
    char* out = s_b64;
    int len = SSD1680_FB_SIZE;
    while (len >= 3) {
        uint32_t v = (uint32_t)(in[0] << 16) | (uint32_t)(in[1] << 8) | in[2];
        *out++ = b64_tab[(v >> 18) & 0x3F];
        *out++ = b64_tab[(v >> 12) & 0x3F];
        *out++ = b64_tab[(v >> 6) & 0x3F];
        *out++ = b64_tab[v & 0x3F];
        in += 3;
        len -= 3;
    }
    if (len == 1) { /* 3904 % 3 == 1: this branch is the live one */
        uint32_t v = (uint32_t)(in[0] << 16);
        *out++ = b64_tab[(v >> 18) & 0x3F];
        *out++ = b64_tab[(v >> 12) & 0x3F];
        *out++ = '=';
        *out++ = '=';
    } else if (len == 2) {
        uint32_t v = (uint32_t)(in[0] << 16) | (uint32_t)(in[1] << 8);
        *out++ = b64_tab[(v >> 18) & 0x3F];
        *out++ = b64_tab[(v >> 12) & 0x3F];
        *out++ = b64_tab[(v >> 6) & 0x3F];
        *out++ = '=';
    }
    *out = '\0';
}

/* ── public API ──────────────────────────────────────────────────────── */

int display_init(void) {
    ssd1680_engine_init();
    s_seq = 0;
    s_initialized = true;
    return 0;
}

void display_flush(void) {
    if (!s_initialized)
        return;

    const ssd1680_op_t* ops;
    size_t n_ops;
    uint32_t busy_ms;
    ssd1680_refresh_t kind = ssd1680_engine_flush(&ops, &n_ops, &busy_ms);
    if (kind == SSD1680_REFRESH_NONE)
        return;
    (void)ops; /* the broker gets the resolved fb, not the SPI stream */
    (void)n_ops;

    fb_to_b64(ssd1680_engine_fb());

    cJSON* msg = cJSON_CreateObject();
    if (!msg)
        return;
    cJSON_AddStringToObject(msg, "t", "fb");
    cJSON_AddNumberToObject(msg, "seq", (double)++s_seq);
    cJSON_AddStringToObject(msg, "kind",
                            kind == SSD1680_REFRESH_FULL ? "full" : "partial");
    cJSON_AddStringToObject(msg, "fb", s_b64);
    cJSON_AddNumberToObject(msg, "busy_ms", (double)busy_ms);
    /* emu_link_send takes ownership of msg on all paths and drops the
     * message silently when no broker is attached. */
    emu_link_send(msg);
}

/* ── framebuffer drawing (engine-backed, mirrors ssd1680_io.c) ───────── */

void display_pixel(int x, int y, bool on) {
    if (s_rotated_180) {
        x = DISPLAY_WIDTH - 1 - x;
        y = DISPLAY_HEIGHT - 1 - y;
    }
    ssd1680_engine_pixel(x, y, on); /* engine clips out-of-range */
}

void display_clear(void) {
    for (int y = 0; y < DISPLAY_HEIGHT; y++)
        for (int x = 0; x < DISPLAY_WIDTH; x++)
            ssd1680_engine_pixel(x, y, false);
}

void display_fill(void) {
    for (int y = 0; y < DISPLAY_HEIGHT; y++)
        for (int x = 0; x < DISPLAY_WIDTH; x++)
            ssd1680_engine_pixel(x, y, true);
}

void display_hline(int x, int y, int w) {
    for (int i = 0; i < w; i++)
        display_pixel(x + i, y, true);
}

void display_draw_text(int x, int y, const char* text) {
    while (*text) {
        uint8_t c = (uint8_t)*text;
        if (c >= 0x20 && c <= 0x7E) {
            const uint8_t* glyph = font6x8[c - 0x20];
            for (int col = 0; col < 6; col++) {
                uint8_t bits = glyph[col];
                for (int row = 0; row < 8; row++) {
                    if (bits & (1 << row))
                        display_pixel(x + col, y + row, true);
                }
            }
        }
        x += 6;
        if (x >= DISPLAY_WIDTH)
            break;
        text++;
    }
}

void display_draw_text_large(int x, int y, const char* text) {
    while (*text) {
        uint8_t c = (uint8_t)*text;
        if (c >= 0x20 && c <= 0x7E) {
            const uint8_t* glyph = font6x8[c - 0x20];
            for (int col = 0; col < 6; col++) {
                uint8_t bits = glyph[col];
                for (int row = 0; row < 8; row++) {
                    if (bits & (1 << row)) {
                        display_pixel(x + col * 2, y + row * 2, true);
                        display_pixel(x + col * 2 + 1, y + row * 2, true);
                        display_pixel(x + col * 2, y + row * 2 + 1, true);
                        display_pixel(x + col * 2 + 1, y + row * 2 + 1, true);
                    }
                }
            }
        }
        x += 12;
        if (x >= DISPLAY_WIDTH)
            break;
        text++;
    }
}

/* ── capability shims (same surface as ssd1680_io.c) ─────────────────── */

void display_power(bool on) { (void)on; }

void display_set_backlight(uint8_t level) { (void)level; }

uint8_t display_get_backlight(void) { return 255; }

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
