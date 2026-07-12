/*
 * SSD1680 e-paper transport for the Bramble Pager v1 (GDEY0213B74 2.13"
 * 250x122 panel). Implements display.h on top of the pure command-stream
 * engine (ssd1680_engine.c): this file only moves bytes.
 *
 * Electrical contract (GDEY0213B74 datasheet p.8 notes 5-1..5-5):
 *   - 4-wire SPI, BS1 grounded on the board. CS active low (handled by
 *     the SPI driver via spics_io_num).
 *   - D/C# high = data, low = command.
 *   - RES# active low. A reset pulse also wakes the controller from deep
 *     sleep, which the engine enters at the end of every flush.
 *   - BUSY active HIGH: while high, no command may be sent (note 5-4).
 *     All waits carry a timeout so a missing/unfitted panel cannot hang
 *     boot forever (this driver ships before the boards do).
 *
 * The SPI bus is shared with the SX1262 radio (BOARD_CAP_SHARED_SPI:
 * board_init owns the bus and g_spi_mutex); the whole flush, including
 * the post-activation BUSY wait, runs under g_spi_mutex so radio command
 * sequences cannot interleave with the panel's.
 */
#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)

#include "include/display.h"
#include "include/font_6x8.h"
#include "include/ssd1680_engine.h"
#include "board_config.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char* TAG = "ssd1680";
static const bramble_board_config_t* s_board = NULL;
static spi_device_handle_t s_spi = NULL;
static bool s_initialized = false;
static bool s_rotated_180 = false;

/* SSD1680 max write SCL is 20 MHz; run at 10 MHz for margin on the shared
 * bus. Chunk data below the board's spi_max_transfer_sz (256 on pager). */
#define EPD_SPI_HZ (10 * 1000 * 1000)
#define EPD_SPI_CHUNK 240

/* BUSY-wait budgets: command processing (reset, register writes) is fast;
 * only master activation runs for seconds, bounded by the engine's
 * busy_ms plus margin. */
#define EPD_BUSY_CMD_TIMEOUT_MS 2000
#define EPD_BUSY_REFRESH_MARGIN_MS 2000
#define EPD_BUSY_POLL_MS 10

/* ── low-level helpers ───────────────────────────────────────────────── */

static int epd_wait_busy(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (gpio_get_level(s_board->epd_display.busy)) { /* active HIGH */
        if (waited >= timeout_ms) {
            ESP_LOGW(TAG, "BUSY still high after %lu ms", (unsigned long)timeout_ms);
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(EPD_BUSY_POLL_MS));
        waited += EPD_BUSY_POLL_MS;
    }
    return 0;
}

static void epd_reset_pulse(void) {
    /* RES# low pulse; 10 ms per the typical operating sequence (p.31:
     * HW reset, wait 10 ms). Also the only way out of deep sleep. */
    gpio_set_level(s_board->epd_display.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_board->epd_display.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static esp_err_t epd_write_cmd(uint8_t cmd) {
    gpio_set_level(s_board->epd_display.dc, 0);
    /* The command byte fits inline: SPI_TRANS_USE_TXDATA skips the
     * per-transaction DMA bounce alloc a flash-resident tx_buffer forces. */
    spi_transaction_t t = {.length = 8, .flags = SPI_TRANS_USE_TXDATA};
    t.tx_data[0] = cmd;
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "cmd 0x%02x transmit failed: %s", cmd, esp_err_to_name(err));
    return err;
}

static esp_err_t epd_write_data(uint8_t cmd, const uint8_t* data, size_t len) {
    if (len == 0)
        return ESP_OK;
    gpio_set_level(s_board->epd_display.dc, 1);
    while (len > 0) {
        size_t chunk = len > EPD_SPI_CHUNK ? EPD_SPI_CHUNK : len;
        spi_transaction_t t = {.length = chunk * 8};
        if (chunk <= 4) {
            /* Short const register payloads (every non-RAM op): inline into
             * the transaction, killing the hidden DMA bounce alloc that a
             * flash-resident tx_buffer would need. RAM writes stay large and
             * take the buffer-pointer path below. */
            t.flags = SPI_TRANS_USE_TXDATA;
            memcpy(t.tx_data, data, chunk);
        } else {
            t.tx_buffer = data;
        }
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cmd 0x%02x data transmit failed: %s", cmd, esp_err_to_name(err));
            return err;
        }
        data += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

/* ── public API ──────────────────────────────────────────────────────── */

int display_init(void) {
    s_board = board_get_config();

    if (!(s_board->capabilities & BOARD_CAP_DISPLAY_EPAPER)) {
        ESP_LOGW(TAG, "E-paper display not supported on this board");
        return -1;
    }

    /* D/C and RES# outputs; BUSY input. CS belongs to the SPI driver. */
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << s_board->epd_display.dc) | (1ULL << s_board->epd_display.rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t gerr = gpio_config(&out_conf);
    if (gerr != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config (DC/RST) failed: %s", esp_err_to_name(gerr));
        return -1;
    }
    gpio_set_level(s_board->epd_display.rst, 1); /* not in reset */

    gpio_config_t busy_conf = {
        .pin_bit_mask = (1ULL << s_board->epd_display.busy),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gerr = gpio_config(&busy_conf);
    if (gerr != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config (BUSY) failed: %s", esp_err_to_name(gerr));
        return -1;
    }

    /* Attach to the shared SPI bus initialized by board_init. */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = s_board->epd_display.cs,
        .queue_size = 1,
    };
    esp_err_t err = spi_bus_add_device(s_board->spi_host, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(err));
        return -1;
    }

    ssd1680_engine_init();
    s_initialized = true;

    /* No panel traffic here: the engine's first flush carries the whole
     * init sequence (p.31) and is forced FULL. Boot stays fast and a
     * missing panel costs nothing until the first flush. */
    ESP_LOGI(TAG, "SSD1680 250x122 e-paper ready (CS=%d DC=%d RST=%d BUSY=%d)",
             s_board->epd_display.cs, s_board->epd_display.dc, s_board->epd_display.rst,
             s_board->epd_display.busy);
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

    if (g_spi_mutex)
        xSemaphoreTake(g_spi_mutex, portMAX_DELAY);

    /* Wake from deep sleep (every stream ends with 0x10, so every flush
     * starts asleep) and wait until the controller is ready. */
    epd_reset_pulse();
    if (epd_wait_busy(EPD_BUSY_CMD_TIMEOUT_MS) != 0)
        goto out; /* no panel? skip quietly; bring-up flag, not a crash */

    bool refreshing = false; /* previous op was master activation */
    for (size_t i = 0; i < n_ops; i++) {
        /* Never send a command while BUSY is high (p.8 note 5-4). */
        if (refreshing) {
            /* The panel drives its waveform for busy_ms (seconds on a
             * full refresh) and does not need the SPI bus meanwhile:
             * CS is idle, so release g_spi_mutex and let the radio run,
             * then retake it for the remaining ops (the deep-sleep op). */
            if (g_spi_mutex)
                xSemaphoreGive(g_spi_mutex);
            int rc = epd_wait_busy(busy_ms + EPD_BUSY_REFRESH_MARGIN_MS);
            if (g_spi_mutex)
                xSemaphoreTake(g_spi_mutex, portMAX_DELAY);
            if (rc != 0)
                goto out;
            refreshing = false;
        } else if (epd_wait_busy(EPD_BUSY_CMD_TIMEOUT_MS) != 0) {
            goto out;
        }
        /* A failed transmit leaves the panel with a partial command stream;
         * abort the rest of the ops rather than push more onto a bad state.
         * display_flush is void (display.h contract). Note: the engine already
         * cleared its dirty state before the io layer ran, so an aborted flush
         * is NOT retried; the frame is recovered on the next pixel change. */
        if (epd_write_cmd(ops[i].cmd) != ESP_OK)
            goto out;
        if (epd_write_data(ops[i].cmd, ops[i].data, ops[i].len) != ESP_OK)
            goto out;
        refreshing = (ops[i].cmd == SSD1680_CMD_MASTER_ACTIVATE);
        /* No wait after the final deep-sleep op: BUSY stays high in deep
         * sleep and the panel is done until the next flush. */
    }

out:
    if (g_spi_mutex)
        xSemaphoreGive(g_spi_mutex);
}

/* ── framebuffer drawing (engine-backed) ─────────────────────────────── */

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

/* ── capability shims ────────────────────────────────────────────────── */

void display_power(bool on) {
    /* The engine parks the panel in deep sleep after every flush; there
     * is no separate on/off rail to drive. */
    (void)on;
}

void display_set_backlight(uint8_t level) { (void)level; /* no backlight */ }

uint8_t display_get_backlight(void) { return 255; }

void display_set_contrast(uint8_t val) { (void)val; /* not applicable */ }

void display_invert(bool invert) { (void)invert; /* no cheap runtime invert on e-paper */ }

void display_set_rotated_180(bool rotated) { s_rotated_180 = rotated; }

bool display_get_rotated_180(void) { return s_rotated_180; }

void display_flush_area(int x1, int y1, int x2, int y2, const uint16_t* buf) {
    /* LVGL path; the pager UI does not use it. */
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)buf;
}

int display_get_width(void) { return DISPLAY_WIDTH; }

int display_get_height(void) { return DISPLAY_HEIGHT; }

#endif /* ESP_PLATFORM && !CONFIG_IDF_TARGET_LINUX */
