/*
 * Virtual SSD1306 OLED backend for the emulator (IDF linux target).
 *
 * The OLED analogue of display_virt.c. Where display_virt.c drives the pure
 * SSD1680 e-paper engine and streams its 250x122 logical framebuffer, this
 * backend owns a plain 128x64 1bpp framebuffer (what a real SSD1306 shows) and
 * ships it to the emu-link broker as the same "fb" message:
 *   { t:"fb", seq, kind:"full", w:128, h:64, fb:<base64 1024 B>, busy_ms:0 }
 *
 * An OLED has no refresh physics: every flush is an immediate, full update, so
 * kind is always "full" and busy_ms is always 0 (there is no panel-busy
 * window). The frontend (simulator/ui Oled.tsx) renders the bytes directly.
 *
 * fb payload layout (identical bit convention to the e-paper logical fb, only
 * the geometry differs): 128x64 1bpp, row-major, 16 bytes per row (128 px ->
 * exactly 16 bytes, no pad bits), MSB of each byte is the leftmost pixel of its
 * 8-pixel group, bit set = a lit (foreground) pixel. render_screen() in
 * main/main.c draws into this through display_draw_text() exactly as it does on
 * the e-paper, so the same text UI lands here byte-for-byte at 128x64.
 */
#include "display.h"
#include "font_6x8.h"
#include "emu_link.h"
#include "cJSON.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define OLED_STRIDE ((DISPLAY_WIDTH + 7) / 8)       /* 16 for 128 px */
#define OLED_FB_SIZE (OLED_STRIDE * DISPLAY_HEIGHT) /* 1024 for 128x64 */

static bool s_initialized = false;
static bool s_rotated_180 = false;
static uint32_t s_seq = 0; /* monotonically increasing, first frame = 1 */

/* The logical framebuffer, plus the last frame actually sent, so an unchanged
 * flush is a no-op (mirrors the e-paper engine's REFRESH_NONE: the frontend is
 * never handed a duplicate frame). */
static uint8_t s_fb[OLED_FB_SIZE];
static uint8_t s_sent_fb[OLED_FB_SIZE];
static bool s_ever_sent = false;

/* ── base64 (standard alphabet, padded) ──────────────────────────────── */

static const char b64_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#define FB_B64_LEN (((OLED_FB_SIZE + 2) / 3) * 4)
static char s_b64[FB_B64_LEN + 1];

static void fb_to_b64(const uint8_t* in) {
    char* out = s_b64;
    int len = OLED_FB_SIZE;
    while (len >= 3) {
        uint32_t v = (uint32_t)(in[0] << 16) | (uint32_t)(in[1] << 8) | in[2];
        *out++ = b64_tab[(v >> 18) & 0x3F];
        *out++ = b64_tab[(v >> 12) & 0x3F];
        *out++ = b64_tab[(v >> 6) & 0x3F];
        *out++ = b64_tab[v & 0x3F];
        in += 3;
        len -= 3;
    }
    /* OLED_FB_SIZE (1024) % 3 == 1: the len==1 tail is the live branch. */
    if (len == 1) {
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
    memset(s_fb, 0, sizeof(s_fb));
    memset(s_sent_fb, 0, sizeof(s_sent_fb));
    s_ever_sent = false;
    s_seq = 0;
    s_initialized = true;
    return 0;
}

/* Virtual OLED has no e-paper-style residue: every flush ships the whole
 * frame, so there is nothing to clear. */
void display_request_full_refresh(void) {}

void display_flush(void) {
    if (!s_initialized)
        return;

    /* Unchanged since the last emitted frame: nothing to send. First flush
     * always sends so the panel shows its boot content. */
    if (s_ever_sent && memcmp(s_fb, s_sent_fb, OLED_FB_SIZE) == 0)
        return;

    fb_to_b64(s_fb);
    memcpy(s_sent_fb, s_fb, OLED_FB_SIZE);
    s_ever_sent = true;

    cJSON* msg = cJSON_CreateObject();
    if (!msg)
        return;
    cJSON_AddStringToObject(msg, "t", "fb");
    cJSON_AddNumberToObject(msg, "seq", (double)++s_seq);
    cJSON_AddStringToObject(msg, "kind", "full"); /* OLED: always a full, immediate update */
    cJSON_AddStringToObject(msg, "fb", s_b64);
    cJSON_AddNumberToObject(msg, "w", (double)DISPLAY_WIDTH);
    cJSON_AddNumberToObject(msg, "h", (double)DISPLAY_HEIGHT);
    cJSON_AddNumberToObject(msg, "busy_ms", 0.0); /* no panel-busy window on an OLED */
    /* emu_link_send takes ownership of msg on all paths and drops the
     * message silently when no broker is attached. */
    emu_link_send(msg);
}

/* ── framebuffer drawing (mirrors display_virt.c / ssd1306.c blit) ────── */

void display_pixel(int x, int y, bool on) {
    if (s_rotated_180) {
        x = DISPLAY_WIDTH - 1 - x;
        y = DISPLAY_HEIGHT - 1 - y;
    }
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT)
        return; /* clip out-of-range, same as the engine */
    uint8_t* byte = &s_fb[y * OLED_STRIDE + (x >> 3)];
    uint8_t mask = (uint8_t)(0x80 >> (x & 7)); /* MSB = leftmost pixel */
    if (on)
        *byte |= mask;
    else
        *byte &= (uint8_t)~mask;
}

void display_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

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

/* ── capability shims (same surface as ssd1306.c) ─────────────────────── */

void display_power(bool on) { (void)on; }

void display_set_backlight(uint8_t level) { (void)level; }

uint8_t display_get_backlight(void) { return 255; }

void display_set_rotated_180(bool rotated) { s_rotated_180 = rotated; }

bool display_get_rotated_180(void) { return s_rotated_180; }

void display_flush_area(int x1, int y1, int x2, int y2, const uint16_t* buf) {
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)buf;
}
