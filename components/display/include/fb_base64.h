#pragma once

#include <stdint.h>

/* Standard-alphabet, padded base64 for framebuffer and screenshot payloads.
 *
 * This is the tree's only base64 encoder, so no caller carries a copy of its
 * own. The linux-target emulator builds one of two mutually exclusive virtual
 * display backends (display_virt.c for the SSD1680 e-paper, display_virt_oled.c
 * for the SSD1306 OLED) and ships each whole framebuffer to the emu-link
 * frontend as a base64-encoded "fb" string; the screenshot RPC in
 * main/rpc_methods.c encodes one chunk of a captured frame per call.
 *
 * Encodes `len` bytes from `in` into `out`, which must hold at least
 * ((len + 2) / 3) * 4 + 1 bytes, and writes a trailing NUL. Every call emits an
 * independent, self-contained string with its own padding and no line breaks,
 * so a chunked caller decodes chunk by chunk and concatenates the raw bytes
 * without tracking bit alignment across calls. */
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
