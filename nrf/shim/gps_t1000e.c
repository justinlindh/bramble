/*
 * AG3335 GNSS driver for the T1000-E, satisfying components/gps/include/gps.h
 * over UARTE1. Event-driven (unlike console_uart.c's blocking UARTE0): an
 * nrfx event handler feeds raw bytes into a stream buffer, a dedicated task
 * drains it into gps_feed, and the feed's fix callback (gps_events.c) emits
 * the RPC event.
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
#include <task.h>

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
static uint8_t s_rx_buf[2][64];       /* alternating EasyDMA targets */
static StreamBufferHandle_t s_stream; /* 512B, ISR -> task */
static TaskHandle_t s_task;           /* "gnss", prio 5 */
static SemaphoreHandle_t s_mu;        /* guards s_feed */
static SemaphoreHandle_t s_tx_done;   /* UARTE TX completion */
static gps_feed_t s_feed;
static volatile uint32_t s_rx_overruns; /* stream-buffer-full drops */
static bool s_powered;                  /* EN state */
static bool s_user_enabled;             /* mirrors gps_pref at runtime */

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
        break;
    }
    case NRFX_UARTE_EVT_ERROR: {
        uint8_t* buf = event->data.error.rx.p_buffer ? event->data.error.rx.p_buffer : s_rx_buf[0];
        (void)nrfx_uarte_rx(&s_uarte, buf, sizeof(s_rx_buf[0]));
        break;
    }
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
 * flash, so every send copies through this static buffer first. Callers
 * wait for TX_DONE before returning, so the buffer is never reused while a
 * transfer is in flight. */
static void gnss_tx(const char* cmd, size_t len) {
    static uint8_t s_tx_buf[40];
    if (len == 0 || len > sizeof(s_tx_buf)) {
        ESP_LOGE(TAG, "gnss_tx: bad length %u", (unsigned)len);
        return;
    }
    memcpy(s_tx_buf, cmd, len);
    if (nrfx_uarte_tx(&s_uarte, s_tx_buf, len, 0) != NRFX_SUCCESS) {
        ESP_LOGW(TAG, "gnss_tx: nrfx_uarte_tx failed");
        return;
    }
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "gnss_tx: TX_DONE timeout");
    }
}

#define GNSS_TX_STR(s) gnss_tx((s), sizeof(s) - 1)

/* ------------------------------------------------------------------ */
/*  Power sequencing                                                    */
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
 * beats optimal, and the commands are idempotent. */
static void gnss_power_on(void) {
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

    s_powered = true;
    ESP_LOGI(TAG, "GNSS powered on");
}

static void gnss_power_off(void) {
    /* EN LOW; VRTC stays HIGH (never cleared). */
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_EN);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool had = gps_feed_has_fix(&s_feed);
    gps_feed_reset(&s_feed);
    xSemaphoreGive(s_mu);

    if (had) {
        nrf_gps_emit_fix_lost();
    }
    s_powered = false;
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
        size_t n = xStreamBufferReceive(s_stream, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        if (n > 0) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            gps_feed_bytes(&s_feed, chunk, n, now_ms());
            xSemaphoreGive(s_mu);
        }

        TickType_t now_tick = xTaskGetTickCount();
        if ((now_tick - last_duty_tick) >= pdMS_TO_TICKS(1000)) {
            last_duty_tick = now_tick;
            /* Task 9: duty-cycle evaluation hooks in here once it lands. */
        }
    }
}

/* ------------------------------------------------------------------ */
/*  gps.h                                                               */
/* ------------------------------------------------------------------ */

int gps_init(gps_fix_cb_t cb, void* ctx) {
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

    if (xTaskCreate(gnss_task, "gnss", 2560, NULL, 5, &s_task) != pdPASS) {
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

    s_user_enabled = true;
    gnss_power_on();
    ESP_LOGI(TAG, "gps_init complete");
    return 0;
}

int gps_set_enabled(bool enabled) {
    if (s_task == NULL) {
        return -1;
    }
    if (enabled) {
        s_user_enabled = true;
        if (!s_powered) {
            gnss_power_on();
        }
    } else {
        s_user_enabled = false;
        if (s_powered) {
            gnss_power_off();
        }
    }
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
}

void gps_deinit(void) {
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    nrfx_uarte_uninit(&s_uarte);
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_EN);
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        gps_feed_reset(&s_feed);
        xSemaphoreGive(s_mu);
    } else {
        gps_feed_reset(&s_feed);
    }
    s_powered = false;
}
