#include "console.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#include <nrfx_uarte.h>

#include "esp_log.h"
#include "wio_wm1110_devkit.h"

// The console pins are a board fact (bench-determined CH340 route); see the
// board header.
#define CONSOLE_TX_PIN BOARD_PIN_CONSOLE_TX
#define CONSOLE_RX_PIN BOARD_PIN_CONSOLE_RX

static nrfx_uarte_t s_uarte = NRFX_UARTE_INSTANCE(0);
static bool s_ready;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

void console_init(void) {
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);

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

    /*
     * One UARTE, many callers. nrfx_uarte_tx returns BUSY rather than queuing
     * when a transfer is already in flight, so concurrent loggers silently
     * lose whole lines and truncate each other mid-word. That turns the
     * console into an unreliable witness exactly when it matters most: while
     * chasing a BLE pairing failure, the lines that named the failure were
     * the ones being dropped. Serialize instead.
     */
    bool locked = s_lock != NULL && !xPortIsInsideInterrupt() &&
                  xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
    if (locked) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    // EasyDMA requires a RAM source; callers pass stack or static buffers,
    // both of which are RAM on this target.
    (void)nrfx_uarte_tx(&s_uarte, (const uint8_t*)buf, len, 0);
    if (locked) {
        xSemaphoreGive(s_lock);
    }
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
