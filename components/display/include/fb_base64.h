#pragma once

#include <stdint.h>

/* Standard-alphabet, padded base64 for the virtual display backends.
 *
 * The linux-target emulator builds one of two mutually exclusive virtual
 * backends (display_virt.c for the SSD1680 e-paper, display_virt_oled.c for the
 * SSD1306 OLED) and ships each framebuffer to the emu-link frontend as a
 * base64-encoded "fb" string. This is the ONE encoder both backends use, so
 * neither carries a copy of its own; the framebuffer size is the only thing
 * that differs between them, and it is passed here as `len`.
 *
 * Encodes `len` bytes from `in` into `out`, which must hold at least
 * ((len + 2) / 3) * 4 + 1 bytes, and writes a trailing NUL. Both live
 * framebuffer sizes (3904 and 1024) are 1 mod 3, so the len == 1 tail is the
 * exercised padding branch. */
static inline void fb_base64_encode(const uint8_t* in, int len, char* out) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    while (len >= 3) {
        uint32_t v = (uint32_t)(in[0] << 16) | (uint32_t)(in[1] << 8) | in[2];
        *out++ = tab[(v >> 18) & 0x3F];
        *out++ = tab[(v >> 12) & 0x3F];
        *out++ = tab[(v >> 6) & 0x3F];
        *out++ = tab[v & 0x3F];
        in += 3;
        len -= 3;
    }
    if (len == 1) {
        uint32_t v = (uint32_t)(in[0] << 16);
        *out++ = tab[(v >> 18) & 0x3F];
        *out++ = tab[(v >> 12) & 0x3F];
        *out++ = '=';
        *out++ = '=';
    } else if (len == 2) {
        uint32_t v = (uint32_t)(in[0] << 16) | (uint32_t)(in[1] << 8);
        *out++ = tab[(v >> 18) & 0x3F];
        *out++ = tab[(v >> 12) & 0x3F];
        *out++ = tab[(v >> 6) & 0x3F];
        *out++ = '=';
    }
    *out = '\0';
}
