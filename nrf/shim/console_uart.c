#include "console.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <nrfx_uarte.h>

#include "esp_log.h"
#include "wio_wm1110_devkit.h"

// The console pins are a board fact (bench-determined CH340 route); see the
// board header.
#define CONSOLE_TX_PIN BOARD_PIN_CONSOLE_TX
#define CONSOLE_RX_PIN BOARD_PIN_CONSOLE_RX

static nrfx_uarte_t s_uarte = NRFX_UARTE_INSTANCE(0);
static bool s_ready;

void console_init(void) {
    nrfx_uarte_config_t cfg = NRFX_UARTE_DEFAULT_CONFIG(CONSOLE_TX_PIN, CONSOLE_RX_PIN);
    cfg.baudrate = NRF_UARTE_BAUDRATE_115200;
    // No event handler: the driver runs in blocking mode.
    if (nrfx_uarte_init(&s_uarte, &cfg, NULL) == NRFX_SUCCESS) {
        s_ready = true;
    }
}

void console_write(const char* buf, size_t len) {
    if (!s_ready || len == 0) {
        return;
    }
    // EasyDMA requires a RAM source; callers pass stack or static buffers,
    // both of which are RAM on this target.
    (void)nrfx_uarte_tx(&s_uarte, (const uint8_t*)buf, len, 0);
}

void bramble_log_write(char level, const char* tag, const char* fmt, ...) {
    char line[192];
    int n = snprintf(line, sizeof(line), "%c (%lu) %s: ", level,
                     (unsigned long)bramble_log_timestamp_ms(), tag);
    if (n < 0 || n >= (int)sizeof(line)) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0) {
        return;
    }
    size_t used = (size_t)n + (size_t)m;
    if (used > sizeof(line) - 2) {
        used = sizeof(line) - 2;
    }
    line[used++] = '\r';
    line[used++] = '\n';
    console_write(line, used);
}
