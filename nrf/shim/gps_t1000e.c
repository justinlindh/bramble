/*
 * AG3335 GNSS driver for the T1000-E, satisfying components/gps/include/gps.h
 * over UARTE1. Event-driven (unlike console_uart.c's blocking UARTE0): an
 * nrfx event handler feeds raw bytes into a stream buffer, a dedicated task
 * drains it into gps_feed, and the feed's fix callback (gps_events.c) emits
 * the RPC event.
 *
 * Every power transition (the initial power-on and every later
 * gps_set_enabled() toggle) runs on the gnss task, never on the calling
 * task: gps_init() and gps_set_enabled() only set s_user_enabled and wake
 * the task, then return immediately, so neither the boot path nor the RPC
 * handler that services setGpsEnabled ever blocks on the AG3335's ~1.55s
 * power-on sequence. This also makes s_powered task-private (only the gnss
 * task ever reads or writes it), which is what rules out a check-then-act
 * race on it entirely, rather than needing a lock around it.
 *
 * P1.06 (Meshtastic's PIN_3V3_EN, "Power to Sensors") is deliberately NOT
 * driven here. Bench Task 11 Step 2 validates that GNSS runs without it; the
 * documented contingency, if the bench disagrees, is a BOARD_PIN_GNSS_RAIL
 * define plus a step 0 in gnss_power_on(), resolved before merge either way.
 */
#include "gps.h"

#include <string.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <stream_buffer.h>
#include "freertos/task.h" /* the IDF-style path: task.h's shim wrapper gives
                            * xTaskCreate byte-semantics for the stack size
                            * below (see nrf/shim/include/freertos/task.h);
                            * a bare #include <task.h> would silently take
                            * the stack size in words instead. */

#include <hal/nrf_gpio.h>
#include <nrfx_uarte.h>

#include "bramble_board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gps_events.h"
#include "gps_feed.h"
#include "gps_pref.h"

static const char* TAG = "gps_t1000e";

/* Statics */
static nrfx_uarte_t s_uarte = NRFX_UARTE_INSTANCE(1);
static uint8_t s_rx_buf[2][64]; /* alternating EasyDMA targets */
/* Index of the static buffer most recently handed to nrfx_uarte_rx(),
 * whether via the initial queueing, the RX_DONE re-arm, or the
 * RX_BUF_REQUEST re-arm. NRFX_UARTE_EVT_RX_BUF_REQUEST carries no buffer
 * pointer, so this is how its handler knows which of the two is free. */
static uint8_t s_rx_last_idx;
static StreamBufferHandle_t s_stream; /* 512B, ISR -> task */
static TaskHandle_t s_task;           /* "gnss", prio 5 */
static SemaphoreHandle_t s_mu;        /* guards s_feed */
static SemaphoreHandle_t s_tx_done;   /* UARTE TX completion */
static SemaphoreHandle_t s_stopped;   /* gps_deinit's exit-confirmation from gnss_task */
static gps_feed_t s_feed;
static volatile uint32_t s_rx_overruns; /* stream-buffer-full drops */
static volatile uint32_t s_rx_errors;   /* NRFX_UARTE_EVT_ERROR occurrences */
static volatile bool s_shutdown;        /* gps_deinit's request to gnss_task */
static bool s_powered;                  /* EN state; task-private, see file header */
static volatile bool s_user_enabled;    /* mirrors gps_pref/setGpsEnabled; the one
                                         * cross-task input into the gnss task */

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000ULL); }

/* ------------------------------------------------------------------ */
/*  UARTE event handler (IRQ context): only FromISR APIs here.         */
/* ------------------------------------------------------------------ */

static void uarte_handler(nrfx_uarte_event_t const* event, void* context) {
    (void)context;
    BaseType_t woken = pdFALSE;

    switch (event->type) {
    case NRFX_UARTE_EVT_RX_DONE: {
        uint8_t* done_buf = event->data.rx.p_buffer;
        size_t len = event->data.rx.length;
        if (len > 0) {
            size_t sent = xStreamBufferSendFromISR(s_stream, done_buf, len, &woken);
            if (sent < len) {
                s_rx_overruns++;
            }
        }
        /* Re-arm immediately with the buffer that just completed: nrfx
         * already has the other buffer queued as the pending one, so this
         * closes the gap before the FIFO can overrun. */
        (void)nrfx_uarte_rx(&s_uarte, done_buf, sizeof(s_rx_buf[0]));
        s_rx_last_idx = (uint8_t)(done_buf == s_rx_buf[1] ? 1 : 0);
        if (s_task) {
            vTaskNotifyGiveFromISR(s_task, &woken);
        }
        break;
    }
    case NRFX_UARTE_EVT_RX_BUF_REQUEST: {
        /* The driver wants the NEXT buffer queued and this event carries no
         * pointer to say which one is free. With exactly two static buffers
         * rotating between "being filled" and "just resupplied", the free
         * one is always the other of the two relative to whichever was
         * handed out last. This path is reachable in ordinary operation,
         * not just in theory: nrfx's own rxstarted_irq_handler() comment
         * describes a small-transfer interleaving (ENDRX processed, then
         * RXSTARTED processed for that same short transfer) that raises
         * this even though RX_DONE above already resupplies proactively. */
        uint8_t idx = (uint8_t)(s_rx_last_idx ^ 1);
        (void)nrfx_uarte_rx(&s_uarte, s_rx_buf[idx], sizeof(s_rx_buf[0]));
        s_rx_last_idx = idx;
        break;
    }
    case NRFX_UARTE_EVT_ERROR:
        /* No re-arm here. nrfx keeps the receiver running through an ERROR
         * event on its own: irq_handler() processes ERROR and then
         * independently processes ENDRX/RXSTARTED/RX_BUF_REQUEST off the
         * same interrupt; error_irq_handler() only invokes the user
         * handler, it never touches the driver's curr/next buffer state.
         * This event's data.error.rx.p_buffer is also never populated by
         * this nrfx version (user_handler_on_error() only fills
         * error_mask, confirmed against drivers/src/nrfx_uarte.c), so
         * re-arming with it here would always fall back to a hardcoded
         * buffer and could alias one physical buffer as both curr and
         * next. Just count it; RX_DONE and RX_BUF_REQUEST above are the
         * driver's actual supported re-arm paths. */
        s_rx_errors++;
        break;
    case NRFX_UARTE_EVT_TX_DONE:
        xSemaphoreGiveFromISR(s_tx_done, &woken);
        break;
    default:
        break;
    }

    portYIELD_FROM_ISR(woken);
}

/* ------------------------------------------------------------------ */
/*  $PAIR command TX                                                   */
/* ------------------------------------------------------------------ */

/* EasyDMA needs a RAM source; the $PAIR string literals below live in
 * flash, so every send copies through this static buffer first. */
static void gnss_tx(const char* cmd, size_t len) {
    static uint8_t s_tx_buf[40];
    if (len == 0 || len > sizeof(s_tx_buf)) {
        ESP_LOGE(TAG, "gnss_tx: bad length %u", (unsigned)len);
        return;
    }
    /* Drain any stale give left by a prior call's timed-out (and possibly
     * still in-flight) transfer, so a late TX_DONE can never be mistaken
     * for this call's completion. */
    xSemaphoreTake(s_tx_done, 0);
    memcpy(s_tx_buf, cmd, len);
    if (nrfx_uarte_tx(&s_uarte, s_tx_buf, len, 0) != NRFX_SUCCESS) {
        ESP_LOGW(TAG, "gnss_tx: nrfx_uarte_tx failed");
        return;
    }
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "gnss_tx: TX_DONE timeout, aborting");
        /* Synchronous abort: guarantees the transfer, and any DMA read of
         * s_tx_buf, has actually stopped before this returns. Without it
         * the next call's memcpy could race a still-in-flight DMA read of
         * the same static buffer. */
        nrfx_uarte_tx_abort(&s_uarte, true);
    }
}

#define GNSS_TX_STR(s) gnss_tx((s), sizeof(s) - 1)

/* ------------------------------------------------------------------ */
/*  Power sequencing (gnss task only; see file header)                 */
/* ------------------------------------------------------------------ */

static const char* const k_init_cmds[] = {
    "$PAIR066,1,1,1,1,0,0*3A\r\n", /* GPS + GLONASS + GALILEO + BDS */
    "$PAIR062,0,1*3F\r\n",         /* GGA on  */
    "$PAIR062,1,0*3F\r\n",         /* GLL off */
    "$PAIR062,2,0*3C\r\n",         /* GSA off */
    "$PAIR062,3,1*3C\r\n", /* GSV ON: sats_in_view feeds the UI (deviation from Meshtastic) */
    "$PAIR062,4,1*3B\r\n", /* RMC on  */
    "$PAIR062,5,0*3B\r\n", /* VTG off */
    "$PAIR062,6,0*38\r\n", /* ZDA off */
};

/* Re-running the whole sequence on every unpark is deliberate: deterministic
 * beats optimal, and the commands are idempotent. Only ever called from
 * gnss_task(). */
static void gnss_power_on(void) {
    /* Declared powered up front: this function only ever runs on gnss_task,
     * which is the sole reader too, so there is no window where another
     * task could observe a stale s_powered mid-sequence. */
    s_powered = true;

    /* 1. VRTC_EN: set once, NEVER cleared anywhere in this file (grep-able
     * invariant). Buys warm start across power cycles. */
    nrf_gpio_cfg_output(BOARD_PIN_GNSS_VRTC_EN);
    nrf_gpio_pin_set(BOARD_PIN_GNSS_VRTC_EN);

    /* 2. SLEEP_INT output HIGH. */
    nrf_gpio_cfg_output(BOARD_PIN_GNSS_SLEEP_INT);
    nrf_gpio_pin_set(BOARD_PIN_GNSS_SLEEP_INT);

    /* 3. RTC_INT output LOW. */
    nrf_gpio_cfg_output(BOARD_PIN_GNSS_RTC_INT);
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_RTC_INT);

    /* 4. RESETB input pull-up (reset status readback). */
    nrf_gpio_cfg_input(BOARD_PIN_GNSS_RESETB, NRF_GPIO_PIN_PULLUP);

    /* 5. RESET output LOW. */
    nrf_gpio_cfg_output(BOARD_PIN_GNSS_RESET);
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_RESET);

    /* 6. EN output HIGH. */
    nrf_gpio_cfg_output(BOARD_PIN_GNSS_EN);
    nrf_gpio_pin_set(BOARD_PIN_GNSS_EN);

    /* 7. UARTE is already up from gps_init. */

    /* 8. Reset pulse. */
    nrf_gpio_pin_set(BOARD_PIN_GNSS_RESET);
    vTaskDelay(pdMS_TO_TICKS(10));
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_RESET);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 9. Wake: RTC_INT pulse, then $PAIR382 spam for 1000ms (25 sends at
     * 40ms). Meshtastic treats the spam as required on this board; bench
     * Task 11 confirms. */
    nrf_gpio_pin_set(BOARD_PIN_GNSS_RTC_INT);
    vTaskDelay(pdMS_TO_TICKS(3));
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_RTC_INT);
    vTaskDelay(pdMS_TO_TICKS(50));
    for (int i = 0; i < 25; i++) {
        GNSS_TX_STR("$PAIR382,1*2E\r\n");
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    /* 10. Config burst, each line followed by 40ms, then save to flash. */
    for (size_t i = 0; i < sizeof(k_init_cmds) / sizeof(k_init_cmds[0]); i++) {
        gnss_tx(k_init_cmds[i], strlen(k_init_cmds[i]));
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    vTaskDelay(pdMS_TO_TICKS(250));
    GNSS_TX_STR("$PAIR513*3D\r\n");

    ESP_LOGI(TAG, "GNSS powered on");
}

/* Only ever called from gnss_task(). */
static void gnss_power_off(void) {
    s_powered = false;

    /* EN LOW; VRTC stays HIGH (never cleared). */
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_EN);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool had = gps_feed_has_fix(&s_feed);
    gps_feed_reset(&s_feed);
    xSemaphoreGive(s_mu);

    if (had) {
        nrf_gps_emit_fix_lost();
    }
    ESP_LOGI(TAG, "GNSS powered off");
}

/* ------------------------------------------------------------------ */
/*  gnss task                                                           */
/* ------------------------------------------------------------------ */

static void gnss_task(void* arg) {
    (void)arg;
    uint8_t chunk[64];
    TickType_t last_duty_tick = xTaskGetTickCount();

    for (;;) {
        /* Woken by the ISR on RX_DONE (near-instant byte reaction) or by
         * gps_set_enabled()/gps_deinit() on a state change; the 100ms bound
         * is a safety net, not the primary wake path, so quiescent power
         * polling never lags by more than that. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        if (s_shutdown) {
            break;
        }

        size_t n;
        while ((n = xStreamBufferReceive(s_stream, chunk, sizeof(chunk), 0)) > 0) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            gps_feed_bytes(&s_feed, chunk, n, now_ms());
            xSemaphoreGive(s_mu);
        }

        /* The only power-transition call site: see the file header. */
        bool want = s_user_enabled;
        if (want && !s_powered) {
            gnss_power_on();
        } else if (!want && s_powered) {
            gnss_power_off();
        }

        TickType_t now_tick = xTaskGetTickCount();
        if ((now_tick - last_duty_tick) >= pdMS_TO_TICKS(1000)) {
            last_duty_tick = now_tick;
            /* Task 9: duty-cycle evaluation hooks in here once it lands. */
        }
    }

    /* Confirm exit to gps_deinit() before this task is gone: at this point
     * it holds no lock (the loop body above never leaves gnss_power_off()
     * or the byte-drain section mid-way), so gps_deinit() can safely reuse
     * s_mu once it sees this. */
    if (s_stopped) {
        xSemaphoreGive(s_stopped);
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  gps.h                                                               */
/* ------------------------------------------------------------------ */

int gps_init(gps_fix_cb_t cb, void* ctx) {
    s_shutdown = false;

    s_mu = xSemaphoreCreateMutex();
    if (!s_mu) {
        ESP_LOGE(TAG, "gps_init: mutex create failed");
        return -1;
    }
    s_tx_done = xSemaphoreCreateBinary();
    if (!s_tx_done) {
        ESP_LOGE(TAG, "gps_init: tx semaphore create failed");
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
        return -1;
    }
    s_stream = xStreamBufferCreate(512, 1);
    if (!s_stream) {
        ESP_LOGE(TAG, "gps_init: stream buffer create failed");
        vSemaphoreDelete(s_tx_done);
        s_tx_done = NULL;
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
        return -1;
    }

    gps_feed_init(&s_feed, cb, ctx);

    nrfx_uarte_config_t cfg = NRFX_UARTE_DEFAULT_CONFIG(BOARD_PIN_GNSS_TX, BOARD_PIN_GNSS_RX);
    cfg.baudrate = NRF_UARTE_BAUDRATE_115200;
    if (nrfx_uarte_init(&s_uarte, &cfg, uarte_handler) != NRFX_SUCCESS) {
        ESP_LOGE(TAG, "gps_init: nrfx_uarte_init failed");
        vStreamBufferDelete(s_stream);
        s_stream = NULL;
        vSemaphoreDelete(s_tx_done);
        s_tx_done = NULL;
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
        return -1;
    }

    /* Queue both RX buffers: nrfx keeps one active plus one pending, which
     * is what makes the re-arm gap in uarte_handler survivable. */
    if (nrfx_uarte_rx(&s_uarte, s_rx_buf[0], sizeof(s_rx_buf[0])) != NRFX_SUCCESS ||
        nrfx_uarte_rx(&s_uarte, s_rx_buf[1], sizeof(s_rx_buf[1])) != NRFX_SUCCESS) {
        ESP_LOGE(TAG, "gps_init: nrfx_uarte_rx queueing failed");
        nrfx_uarte_uninit(&s_uarte);
        vStreamBufferDelete(s_stream);
        s_stream = NULL;
        vSemaphoreDelete(s_tx_done);
        s_tx_done = NULL;
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
        return -1;
    }
    s_rx_last_idx = 1;

    /* Read the persisted preference here, inside the nRF driver only: the
     * ESP32 gps_init() does not read gps_pref, and gps.h's cross-platform
     * contract is not changing to match, so this stays a T1000-E-specific
     * detail. gps_init() still registers the fix callback either way (ESP
     * boot parity: a later gps_set_enabled(true) works without a second
     * init), it just skips the first power-on when the pref is off. The
     * gnss task below does that skip itself (see gnss_task()'s
     * want/s_powered check), so gps_init() never blocks on the AG3335's
     * power-on sequence regardless of the pref. */
    s_user_enabled = gps_pref_get();

    if (xTaskCreate(gnss_task, "gnss", 3072, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "gps_init: task create failed");
        nrfx_uarte_uninit(&s_uarte);
        vStreamBufferDelete(s_stream);
        s_stream = NULL;
        vSemaphoreDelete(s_tx_done);
        s_tx_done = NULL;
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
        s_task = NULL;
        return -1;
    }

    ESP_LOGI(TAG, "gps_init complete (pref=%s)", s_user_enabled ? "on" : "off");
    return 0;
}

int gps_set_enabled(bool enabled) {
    if (s_task == NULL) {
        return -1;
    }
    s_user_enabled = enabled;
    xTaskNotifyGive(s_task);
    return 0;
}

bool gps_has_fix(void) {
    if (!s_mu) {
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool has_fix = gps_feed_has_fix(&s_feed);
    xSemaphoreGive(s_mu);
    return has_fix;
}

bool gps_get_position(bramble_position_t* out) {
    if (!s_mu) {
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool ok = gps_feed_get_position(&s_feed, out);
    xSemaphoreGive(s_mu);
    return ok;
}

bool gps_get_utc_hm(uint8_t* hour, uint8_t* min) {
    if (!s_mu) {
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool ok = gps_feed_get_utc_hm(&s_feed, hour, min);
    xSemaphoreGive(s_mu);
    return ok;
}

void gps_get_stats(gps_stats_t* out) {
    if (out == NULL) {
        return;
    }
    if (!s_mu) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    gps_feed_get_stats(&s_feed, now_ms(), out);
    xSemaphoreGive(s_mu);
}

void gps_get_debug(gps_debug_t* out) {
    if (out == NULL) {
        return;
    }
    if (!s_mu) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    out->rx_bytes_total = s_feed.rx_bytes_total;
    out->rx_lines_total = s_feed.rx_lines_total;
    strncpy(out->chip, s_feed.chip_banner, sizeof(out->chip) - 1);
    out->chip[sizeof(out->chip) - 1] = '\0';
    xSemaphoreGive(s_mu);
    /* ISR-incremented counters, not gps_feed state: s_mu guards s_feed, not
     * these, so they are read outside the critical section above. Each is a
     * single word (atomic enough on Cortex-M) and merely diagnostic. */
    out->rx_overruns = s_rx_overruns;
    out->rx_errors = s_rx_errors;
}

void gps_deinit(void) {
    if (s_task) {
        TaskHandle_t task = s_task;
        s_shutdown = true;
        SemaphoreHandle_t stopped = xSemaphoreCreateBinary();
        s_stopped = stopped;
        xTaskNotifyGive(task);
        /* 2000ms comfortably covers gnss_task() finishing a worst-case
         * in-progress gnss_power_on() (~1.55s) before it next checks
         * s_shutdown, so the confirmed path below is the expected one. */
        bool confirmed = stopped && xSemaphoreTake(stopped, pdMS_TO_TICKS(2000)) == pdTRUE;
        s_stopped = NULL;
        if (stopped) {
            vSemaphoreDelete(stopped);
        }
        if (confirmed) {
            /* gnss_task() already called vTaskDelete(NULL) on itself after
             * giving the semaphore above, and it held no lock at that
             * point, so there is nothing left to delete and s_mu is safe
             * to reuse below. */
        } else {
            ESP_LOGW(TAG, "gps_deinit: gnss task did not confirm exit in time, forcing delete");
            vTaskDelete(task);
        }
        s_task = NULL;
    }
    nrfx_uarte_uninit(&s_uarte);
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_EN);
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        gps_feed_reset(&s_feed);
        xSemaphoreGive(s_mu);
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
    } else {
        gps_feed_reset(&s_feed);
    }
    if (s_tx_done) {
        vSemaphoreDelete(s_tx_done);
        s_tx_done = NULL;
    }
    if (s_stream) {
        vStreamBufferDelete(s_stream);
        s_stream = NULL;
    }
    s_powered = false;
    s_user_enabled = false;
}
