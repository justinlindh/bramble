#include "gps.h"
#include "board_config.h"
#include <string.h>

/* The POSIX/Linux simulator has no UART driver: it compiles the host stub
 * half below (gps_virt takes over in the emulator later). */
#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "gps_feed.h"
#include <string.h>
#include <ctype.h>

static const char* TAG = "gps";

#define GPS_UART_NUM UART_NUM_1
#define GPS_BUF_SIZE 1024
#define GPS_TASK_STACK_SIZE 4096
#define GPS_TASK_PRIORITY 5

static TaskHandle_t s_gps_task = NULL;
static gps_feed_t s_feed;

static bool gps_probe_nmea_at_baud(int baud, uint32_t probe_ms) {
    ESP_ERROR_CHECK(uart_set_baudrate(GPS_UART_NUM, baud));
    uart_flush_input(GPS_UART_NUM);

    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t good = 0;
    uint32_t total = 0;
    uint8_t data[128];

    while (((uint32_t)(esp_timer_get_time() / 1000ULL) - start_ms) < probe_ms) {
        int len = uart_read_bytes(GPS_UART_NUM, data, sizeof(data), pdMS_TO_TICKS(60));
        if (len <= 0)
            continue;
        for (int i = 0; i < len; i++) {
            uint8_t c = data[i];
            total++;
            if (c == '$' || c == ',' || c == '*' || c == '\r' || c == '\n' || isalnum(c)) {
                good++;
            }
        }
    }

    float ratio = total > 0 ? ((float)good / (float)total) : 0.0f;
    ESP_LOGI(TAG, "GPS baud probe %d: total=%lu nmea_like=%.2f", baud, (unsigned long)total, ratio);
    if (total < 40)
        return false;
    return ratio > 0.85f;
}

static int gps_select_baud(int preferred_baud) {
    const int candidates[] = {preferred_baud, 9600, 38400, 57600, 115200};
    const size_t n = sizeof(candidates) / sizeof(candidates[0]);

    for (size_t i = 0; i < n; i++) {
        int baud = candidates[i];
        bool seen = false;
        for (size_t j = 0; j < i; j++) {
            if (candidates[j] == baud) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;

        if (gps_probe_nmea_at_baud(baud, 1200)) {
            return baud;
        }
    }

    return preferred_baud;
}

/* GPS background task */
static void gps_task(void* arg) {
    (void)arg;
    uint8_t data[128];

    ESP_LOGI(TAG, "GPS task started");
    while (1) {
        int len = uart_read_bytes(GPS_UART_NUM, data, sizeof(data) - 1, pdMS_TO_TICKS(100));
        if (len <= 0)
            continue;
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        bool had_fix = gps_feed_has_fix(&s_feed);
        gps_feed_bytes(&s_feed, data, (size_t)len, now_ms);
        if (!had_fix && gps_feed_has_fix(&s_feed)) {
            bramble_position_t p;
            gps_feed_get_position(&s_feed, &p);
            ESP_LOGI(TAG, "GPS fix acquired: lat=%.6f lon=%.6f alt=%d", p.latitude_e7 / 1e7,
                     p.longitude_e7 / 1e7, p.altitude_m);
        }
    }
}

/* Power the GNSS on, bring up the UART and spawn the parsing task using the
 * callback stashed by gps_init(). Shared by gps_init() and gps_set_enabled()
 * so the runtime toggle re-runs the exact same power-on path. */
static int gps_hw_start(void) {
    const bramble_board_config_t* board = board_get_config();

    if (board->gps.tx < 0 || board->gps.rx < 0) {
        ESP_LOGE(TAG, "GPS pins not configured");
        return -1;
    }

    /* Heltec V4 GNSS control lines (active-low enable, active-low reset). */
    if (board->short_name && strcmp(board->short_name, "heltec_v4") == 0) {
        const int pin_en = 34;      /* GPS_EN active LOW */
        const int pin_reset = 42;   /* GPS_RESET active LOW */
        const int pin_standby = 40; /* force wake when HIGH */

        gpio_config_t gnss_ctrl = {
            .pin_bit_mask = (1ULL << pin_en) | (1ULL << pin_reset) | (1ULL << pin_standby),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&gnss_ctrl);

        gpio_set_level(pin_reset, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(pin_reset, 1);

        gpio_set_level(pin_standby, 1);
        gpio_set_level(pin_en, 0);
        vTaskDelay(pdMS_TO_TICKS(300));

        ESP_LOGI(TAG, "Heltec V4 GNSS ctrl: EN=%d(LOW=on) RESET=%d STANDBY=%d", pin_en, pin_reset,
                 pin_standby);
    }

    /* Bramble Pager GNSS power gate (single P-FET high-side switch, active low). */
    if (board->short_name && strcmp(board->short_name, "bramble_pager") == 0) {
        const int pin_en = 38; /* GNSS_EN, LOW = on (P-FET high-side) */

        gpio_config_t gnss_ctrl = {
            .pin_bit_mask = (1ULL << pin_en),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&gnss_ctrl);

        gpio_set_level(pin_en, 0); /* power GNSS on before the UART probe */
        vTaskDelay(pdMS_TO_TICKS(300));

        ESP_LOGI(TAG, "Bramble Pager GNSS ctrl: EN=%d(LOW=on)", pin_en);
    }

    /* Configure UART */
    uart_config_t uart_config = {
        .baud_rate = board->gps.baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_NUM, GPS_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_NUM, board->gps.tx, board->gps.rx, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    int active_baud = gps_select_baud(board->gps.baud);
    ESP_ERROR_CHECK(uart_set_baudrate(GPS_UART_NUM, active_baud));

    gps_feed_reset(&s_feed);

    ESP_LOGI(TAG, "GPS UART initialized (TX=%d, RX=%d, baud=%d active=%d)", board->gps.tx,
             board->gps.rx, board->gps.baud, active_baud);

    /* Create background task */
    BaseType_t res = xTaskCreate(gps_task, "gps_task", GPS_TASK_STACK_SIZE, NULL, GPS_TASK_PRIORITY,
                                 &s_gps_task);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPS task");
        uart_driver_delete(GPS_UART_NUM);
        return -1;
    }

    ESP_LOGI(TAG, "GPS initialized successfully (cold start may take 30-90s)");
    return 0;
}

/* Drive the pager's GNSS power gate HIGH (P-FET off) without touching the UART
 * task, for the case where GPS is disabled before it was ever started. */
static void gps_pager_power_off(void) {
    const bramble_board_config_t* board = board_get_config();
    if (board->short_name && strcmp(board->short_name, "bramble_pager") == 0) {
        const int pin_en = 38;
        gpio_config_t gnss_ctrl = {
            .pin_bit_mask = (1ULL << pin_en),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&gnss_ctrl);
        gpio_set_level(pin_en, 1); /* GNSS off */
    }
}

int gps_init(gps_fix_cb_t cb, void* ctx) {
    const bramble_board_config_t* board = board_get_config();

    /* Check if board has GPS */
    if (!(board->capabilities & BOARD_CAP_GPS)) {
        ESP_LOGW(TAG, "Board does not support GPS");
        return -1;
    }

    gps_feed_init(&s_feed, cb, ctx);

    return gps_hw_start();
}

int gps_set_enabled(bool enabled) {
    const bramble_board_config_t* board = board_get_config();
    if (!(board->capabilities & BOARD_CAP_GPS)) {
        return -1;
    }

    if (enabled) {
        if (s_gps_task) {
            return 0; /* already running */
        }
        return gps_hw_start();
    }

    if (s_gps_task) {
        gps_deinit(); /* stops the task and cuts GNSS power */
    } else {
        gps_pager_power_off(); /* never started: still ensure the gate is off */
    }
    return 0;
}

bool gps_has_fix(void) { return gps_feed_has_fix(&s_feed); }

bool gps_get_position(bramble_position_t* out) {
    return out && gps_feed_get_position(&s_feed, out);
}

bool gps_get_utc_hm(uint8_t* hour, uint8_t* min) { return gps_feed_get_utc_hm(&s_feed, hour, min); }

void gps_get_stats(gps_stats_t* out) {
    if (!out)
        return;
    gps_feed_get_stats(&s_feed, (uint64_t)(esp_timer_get_time() / 1000ULL), out);
}

void gps_get_debug(gps_debug_t* out) {
    if (!out)
        return;
    out->rx_bytes_total = s_feed.rx_bytes_total;
    out->rx_lines_total = s_feed.rx_lines_total;
    strncpy(out->chip, s_feed.chip_banner, sizeof(out->chip) - 1);
    out->chip[sizeof(out->chip) - 1] = '\0';
    /* This backend has no intermediate buffer to overrun and no distinct
     * error-event channel; the nRF driver is the only one that populates
     * these two counters. */
    out->rx_overruns = 0;
    out->rx_errors = 0;
}

void gps_deinit(void) {
    if (s_gps_task) {
        vTaskDelete(s_gps_task);
        s_gps_task = NULL;
    }
    uart_driver_delete(GPS_UART_NUM);
    gps_feed_reset(&s_feed);

    /* Bramble Pager: cut GNSS power on stop (drive the P-FET gate HIGH = off). */
    const bramble_board_config_t* board = board_get_config();
    if (board->short_name && strcmp(board->short_name, "bramble_pager") == 0) {
        const int pin_en = 38;

        gpio_config_t gnss_ctrl = {
            .pin_bit_mask = (1ULL << pin_en),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&gnss_ctrl);
        gpio_set_level(pin_en, 1); /* GNSS off */
    }
    ESP_LOGI(TAG, "GPS deinitialized");
}

#elif !defined(ESP_PLATFORM)
/* Plain-gcc test harness (no ESP_PLATFORM, e.g. the RPC contract tests that
 * link gps.c directly): minimal no-fix stubs. The IDF linux target does NOT
 * take this branch; there gps_virt.c owns the gps.h implementation (see the
 * #else below), so gps.c and gps_virt.c never both define these symbols. */
int gps_init(gps_fix_cb_t cb, void* ctx) {
    (void)cb;
    (void)ctx;
    return -1;
}
int gps_set_enabled(bool enabled) {
    (void)enabled;
    return -1;
}
bool gps_has_fix(void) { return false; }
bool gps_get_position(bramble_position_t* out) {
    (void)out;
    return false;
}
bool gps_get_utc_hm(uint8_t* hour, uint8_t* min) {
    (void)hour;
    (void)min;
    return false;
}
void gps_get_stats(gps_stats_t* out) {
    if (out) {
        out->sats_used = 0;
        out->sats_in_view = 0;
        out->antenna_warning = false;
    }
}
void gps_get_debug(gps_debug_t* out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}
void gps_deinit(void) {}
#else
/* IDF linux target (ESP_PLATFORM && CONFIG_IDF_TARGET_LINUX): the virtual
 * GPS driver in gps_virt.c provides the gps.h implementation. This file
 * contributes nothing there, so linking gps.c and gps_virt.c together does
 * not clash. */
#endif /* ESP_PLATFORM */
