/**
 * ST7789 320x240 TFT display driver for T-Deck Plus
 * Uses SPI (shared bus with SD card and LoRa radio)
 * 16-bit RGB565 framebuffer in PSRAM
 */

#include "include/display.h"
#include "include/font_6x8.h"
#include "board_config.h"
#include "driver/spi_master.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char* TAG = "st7789";
static const bramble_board_config_t* s_board = NULL;

/* ── Framebuffer ─────────────────────────────────────────────────────── */

/* 320×240 pixels, 16-bit RGB565 format */
#define FB_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2)
static uint16_t* fb = NULL; /* Allocated in PSRAM */

/* SPI device handle */
static spi_device_handle_t spi;
static bool initialized = false;
static bool s_rotated_180 = false; /* API compatibility; no behavior change in Task 1 */
/* Power: the ST7789 backlight LEDs are one of the largest awake-state draws
 * on battery boards (tens of mA at full duty). 200/255 (~78% duty) is barely
 * dimmer to the eye but shaves a proportional slice of that current, so it is
 * the boot default; bramble.setBacklight can still drive the full range. */
#define BACKLIGHT_DEFAULT_DUTY 200

static uint8_t s_backlight_level = BACKLIGHT_DEFAULT_DUTY; /* last level set; wake restore */

/* Color constants (RGB565) */
#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF

/* ── ST7789 Commands ─────────────────────────────────────────────────── */

#define ST7789_SWRESET 0x01
#define ST7789_SLPOUT 0x11
#define ST7789_INVON 0x21
#define ST7789_DISPOFF 0x28
#define ST7789_DISPON 0x29
#define ST7789_CASET 0x2A
#define ST7789_RASET 0x2B
#define ST7789_RAMWR 0x2C
#define ST7789_MADCTL 0x36
#define ST7789_COLMOD 0x3A
#define ST7789_PORCTRL 0xB2
#define ST7789_GCTRL 0xB7
#define ST7789_VCOMS 0xBB
#define ST7789_LCMCTRL 0xC0
#define ST7789_VDVVRHEN 0xC2
#define ST7789_VRH 0xC3
#define ST7789_VDV 0xC4
#define ST7789_FRCTRL2 0xC6
#define ST7789_PWCTRL1 0xD0
#define ST7789_PVGAMCTRL 0xE0
#define ST7789_NVGAMCTRL 0xE1

/* ── SPI Helpers ─────────────────────────────────────────────────────── */

static void st7789_dc_cmd(void) { gpio_set_level(s_board->spi_display.dc, 0); }

static void st7789_dc_data(void) { gpio_set_level(s_board->spi_display.dc, 1); }

static void st7789_write_cmd(uint8_t cmd) {
    st7789_dc_cmd();
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    esp_err_t ret = spi_device_transmit(spi, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI cmd 0x%02X transmit failed: %s", cmd, esp_err_to_name(ret));
    }
}

static void st7789_write_data(const uint8_t* data, size_t len) {
    if (len == 0)
        return;
    st7789_dc_data();
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    esp_err_t ret = spi_device_transmit(spi, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI data transmit (%zu bytes) failed: %s", len, esp_err_to_name(ret));
    }
}

static void st7789_write_byte(uint8_t val) { st7789_write_data(&val, 1); }

/* ── ST7789 Init Sequence (LilyGO T-Deck custom) ────────────────────── */

static void st7789_init_sequence(void) {
    /* Software reset */
    st7789_write_cmd(ST7789_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));

    /* Sleep out */
    st7789_write_cmd(ST7789_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(150));

    /* Normal display mode on */
    st7789_write_cmd(0x13); // NORON

    /* Pixel format: 16-bit RGB565 */
    st7789_write_cmd(ST7789_COLMOD);
    st7789_write_byte(0x55);

    /* Memory access control: landscape (MV, MX), left-to-right for 320×240,
     * RGB subpixel order (BGR bit clear) matching LVGL's RGB565 framebuffer.
     * A wrong subpixel-order bit here is invisible to the bench screenshot
     * RPC, which re-renders upstream of the panel: verification is eyes on
     * the glass. The GT911 touch mapping depends on the MV/MX rotation bits
     * (see touch_gt911.c). */
    st7789_write_cmd(ST7789_MADCTL);
    st7789_write_byte(0x60);

    /* Porch control */
    st7789_write_cmd(ST7789_PORCTRL);
    uint8_t porch[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    st7789_write_data(porch, 5);

    /* Gate control */
    st7789_write_cmd(ST7789_GCTRL);
    st7789_write_byte(0x75);

    /* VCOM setting */
    st7789_write_cmd(ST7789_VCOMS);
    st7789_write_byte(0x1A);

    /* LCM control */
    st7789_write_cmd(ST7789_LCMCTRL);
    st7789_write_byte(0x2C);

    /* VDV and VRH enable */
    st7789_write_cmd(ST7789_VDVVRHEN);
    st7789_write_byte(0x01);

    /* VRH set */
    st7789_write_cmd(ST7789_VRH);
    st7789_write_byte(0x13);

    /* VDV set */
    st7789_write_cmd(ST7789_VDV);
    st7789_write_byte(0x20);

    /* Frame rate control */
    st7789_write_cmd(ST7789_FRCTRL2);
    st7789_write_byte(0x0F);

    /* Power control */
    st7789_write_cmd(ST7789_PWCTRL1);
    uint8_t pwctrl[] = {0xA4, 0xA1};
    st7789_write_data(pwctrl, 2);

    /* Positive gamma correction */
    st7789_write_cmd(ST7789_PVGAMCTRL);
    uint8_t gamma_pos[] = {0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38,
                           0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30};
    st7789_write_data(gamma_pos, 14);

    /* Negative gamma correction */
    st7789_write_cmd(ST7789_NVGAMCTRL);
    uint8_t gamma_neg[] = {0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37,
                           0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x3E};
    st7789_write_data(gamma_neg, 14);

    /* Invert on */
    st7789_write_cmd(ST7789_INVON);

    /* Set initial window (portrait orientation) */
    st7789_write_cmd(ST7789_CASET);
    uint8_t init_caset[] = {0x00, 0x00, 0x00, 0xEF}; // 0-239 (portrait column)
    st7789_write_data(init_caset, 4);

    st7789_write_cmd(ST7789_RASET);
    uint8_t init_raset[] = {0x00, 0x00, 0x01, 0x3F}; // 0-319 (portrait row)
    st7789_write_data(init_raset, 4);

    /* Display on */
    vTaskDelay(pdMS_TO_TICKS(120));
    st7789_write_cmd(ST7789_DISPON);
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* ── Public API ──────────────────────────────────────────────────────── */

int display_init(void) {
    /* Get board configuration */
    s_board = board_get_config();

    /* Only ST7789 boards (T-Deck Plus) */
    if (!(s_board->capabilities & BOARD_CAP_DISPLAY_ST7789)) {
        ESP_LOGW(TAG, "ST7789 display not supported on this board");
        return -1;
    }

    /* Allocate framebuffer in PSRAM */
    fb = heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM);
    if (!fb) {
        ESP_LOGW(TAG, "PSRAM allocation failed, trying default heap");
        fb = heap_caps_malloc(FB_SIZE, MALLOC_CAP_DEFAULT);
    }
    if (!fb) {
        ESP_LOGE(TAG, "ST7789: framebuffer allocation failed (%d bytes)", FB_SIZE);
        return -1;
    }
    memset(fb, 0, FB_SIZE); /* Start with black screen */

    /* Configure DC GPIO (command/data select) */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_board->spi_display.dc),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(s_board->spi_display.dc, 1);

    /* Backlight via LEDC PWM: raw gpio_set_level is insufficient to
     * reliably activate the T-Deck Plus backlight IC (LilyGo uses LEDC).
     * Timer 0 / Channel 0 / 10 kHz / 8-bit duty */
    if (s_board->spi_display.backlight >= 0) {
        ledc_timer_config_t bl_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_num = LEDC_TIMER_0,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .freq_hz = 10000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&bl_timer);

        ledc_channel_config_t bl_channel = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0,
            .intr_type = LEDC_INTR_DISABLE,
            .gpio_num = s_board->spi_display.backlight,
            .duty = BACKLIGHT_DEFAULT_DUTY, /* power: see BACKLIGHT_DEFAULT_DUTY */
            .hpoint = 0,
        };
        ledc_channel_config(&bl_channel);
        ESP_LOGI(TAG, "Backlight LEDC initialized (GPIO%d, duty=%d/255)",
                 s_board->spi_display.backlight, BACKLIGHT_DEFAULT_DUTY);
    }

    /* Add SPI device (shared bus already initialized by board_init) */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 40 * 1000 * 1000, /* 40 MHz */
        .mode = 0,                          /* SPI mode 0 */
        .spics_io_num = s_board->spi_display.cs,
        .queue_size = 7,
        .pre_cb = NULL,
    };

    esp_err_t ret = spi_bus_add_device(s_board->spi_host, &dev_cfg, &spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        heap_caps_free(fb);
        fb = NULL;
        return -1;
    }

    /* Run init sequence */
    st7789_init_sequence();

    /* Directly clear GRAM before using framebuffer.
     * This ensures no residual noise from previous firmware. */
    {
        st7789_write_cmd(ST7789_CASET);
        uint8_t c[] = {0x00, 0x00, ((DISPLAY_WIDTH - 1) >> 8) & 0xFF, (DISPLAY_WIDTH - 1) & 0xFF};
        st7789_write_data(c, 4);
        st7789_write_cmd(ST7789_RASET);
        uint8_t r[] = {0x00, 0x00, ((DISPLAY_HEIGHT - 1) >> 8) & 0xFF, (DISPLAY_HEIGHT - 1) & 0xFF};
        st7789_write_data(r, 4);
        st7789_write_cmd(ST7789_RAMWR);
        st7789_dc_data();

        /* Send 320×240×2 = 153600 bytes of zeros in small chunks */
        uint8_t zero_buf[512];
        memset(zero_buf, 0, sizeof(zero_buf));
        size_t total = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2;
        spi_device_acquire_bus(spi, portMAX_DELAY);
        for (size_t sent = 0; sent < total; sent += sizeof(zero_buf)) {
            size_t chunk = (total - sent > sizeof(zero_buf)) ? sizeof(zero_buf) : (total - sent);
            spi_transaction_t t = {
                .length = chunk * 8,
                .tx_buffer = zero_buf,
            };
            spi_device_transmit(spi, &t);
        }
        spi_device_release_bus(spi);
        ESP_LOGI(TAG, "GRAM cleared (%zu bytes)", total);
    }

    initialized = true;

    /* Flush black framebuffer to clear screen */
    display_flush();

    ESP_LOGI(TAG, "ST7789 initialized: %dx%d, FB=%d bytes", DISPLAY_WIDTH, DISPLAY_HEIGHT, FB_SIZE);

    ESP_LOGI(TAG, "ST7789 display initialized (320×240, RGB565 framebuffer in PSRAM)");
    return 0;
}

void display_clear(void) {
    if (!fb)
        return;
    memset(fb, 0, FB_SIZE);
}

void display_pixel(int x, int y, bool on) {
    if (!fb)
        return;
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT)
        return;
    fb[y * DISPLAY_WIDTH + x] = on ? COLOR_WHITE : COLOR_BLACK;
}

void display_hline(int x, int y, int w) {
    if (!fb)
        return;
    if (y < 0 || y >= DISPLAY_HEIGHT)
        return;
    int x_start = (x < 0) ? 0 : x;
    int x_end = (x + w >= DISPLAY_WIDTH) ? DISPLAY_WIDTH : x + w;
    for (int i = x_start; i < x_end; i++) {
        fb[y * DISPLAY_WIDTH + i] = COLOR_WHITE;
    }
}

void display_draw_text(int x, int y, const char* text) {
    if (!fb || !text)
        return;

    int x_cursor = x;
    for (const char* p = text; *p; p++) {
        char c = *p;
        if (c < 0x20 || c > 0x7E)
            c = '?';

        const uint8_t* glyph = font6x8[c - 0x20];

        /* Draw 6x8 glyph */
        for (int col = 0; col < 6; col++) {
            for (int row = 0; row < 8; row++) {
                if (glyph[col] & (1 << row)) {
                    int px = x_cursor + col;
                    int py = y + row;
                    if (px >= 0 && px < DISPLAY_WIDTH && py >= 0 && py < DISPLAY_HEIGHT) {
                        fb[py * DISPLAY_WIDTH + px] = COLOR_WHITE;
                    }
                }
            }
        }
        x_cursor += 6;
        if (x_cursor >= DISPLAY_WIDTH)
            break;
    }
}

void display_draw_text_large(int x, int y, const char* text) {
    if (!fb || !text)
        return;

    int x_cursor = x;
    for (const char* p = text; *p; p++) {
        char c = *p;
        if (c < 0x20 || c > 0x7E)
            c = '?';

        const uint8_t* glyph = font6x8[c - 0x20];

        /* Draw 12x16 glyph (2x scaling) */
        for (int col = 0; col < 6; col++) {
            for (int row = 0; row < 8; row++) {
                if (glyph[col] & (1 << row)) {
                    /* Draw 2x2 pixel block */
                    for (int dx = 0; dx < 2; dx++) {
                        for (int dy = 0; dy < 2; dy++) {
                            int px = x_cursor + col * 2 + dx;
                            int py = y + row * 2 + dy;
                            if (px >= 0 && px < DISPLAY_WIDTH && py >= 0 && py < DISPLAY_HEIGHT) {
                                fb[py * DISPLAY_WIDTH + px] = COLOR_WHITE;
                            }
                        }
                    }
                }
            }
        }
        x_cursor += 12;
        if (x_cursor >= DISPLAY_WIDTH)
            break;
    }
}

/* ST7789 LCD has no e-paper-style residue: every flush redraws the whole
 * area from RAM (or LVGL owns the panel entirely, see display_flush_area),
 * so there is nothing to clear. */
void display_request_full_refresh(void) {}

void display_flush(void) {
    if (!fb || !initialized)
        return;

    if (g_spi_mutex)
        xSemaphoreTake(g_spi_mutex, portMAX_DELAY);

    /* Set window to full screen (logical coordinates, MADCTL handles rotation) */
    st7789_write_cmd(ST7789_CASET);
    uint8_t caset[] = {0x00, 0x00, ((DISPLAY_WIDTH - 1) >> 8) & 0xFF, (DISPLAY_WIDTH - 1) & 0xFF};
    st7789_write_data(caset, 4);

    st7789_write_cmd(ST7789_RASET);
    uint8_t raset[] = {0x00, 0x00, ((DISPLAY_HEIGHT - 1) >> 8) & 0xFF, (DISPLAY_HEIGHT - 1) & 0xFF};
    st7789_write_data(raset, 4);

    st7789_write_cmd(ST7789_RAMWR);

    /* Send framebuffer in small chunks via a stack-allocated DMA-safe buffer.
     * PSRAM is not DMA-accessible on ESP32-S3: must copy to internal RAM first.
     * Use stack variable (not static) to guarantee internal RAM placement,
     * since CONFIG_SPIRAM_USE_MALLOC can place statics in PSRAM. */
    st7789_dc_data();
    uint8_t dma_buf[512]; /* Stack-allocated, guaranteed internal SRAM */

    spi_device_acquire_bus(spi, portMAX_DELAY);

    const uint8_t* src = (const uint8_t*)fb;
    size_t total = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2;

    for (size_t sent = 0; sent < total; sent += sizeof(dma_buf)) {
        size_t chunk = (total - sent > sizeof(dma_buf)) ? sizeof(dma_buf) : (total - sent);
        memcpy(dma_buf, src + sent, chunk);
        spi_transaction_t t = {
            .length = chunk * 8,
            .tx_buffer = dma_buf,
        };
        esp_err_t ret = spi_device_transmit(spi, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI flush error at offset %zu: %s", sent, esp_err_to_name(ret));
            break;
        }
    }

    spi_device_release_bus(spi);

    if (g_spi_mutex)
        xSemaphoreGive(g_spi_mutex);
}

void display_power(bool on) {
    if (!initialized)
        return;
    /* DISPOFF/DISPON share the SPI bus with the SX1262 radio. Take the bus
     * mutex so this single-command transaction cannot interleave into the
     * radio's mutex-protected sequence (or an in-flight flush) and wedge it.
     * No caller of display_power() holds g_spi_mutex, so this cannot
     * self-deadlock. */
    if (g_spi_mutex)
        xSemaphoreTake(g_spi_mutex, portMAX_DELAY);
    st7789_write_cmd(on ? ST7789_DISPON : ST7789_DISPOFF);
    if (g_spi_mutex)
        xSemaphoreGive(g_spi_mutex);
}

void display_set_backlight(uint8_t level) {
    /* level 0 = off, 255 = full brightness */
    s_backlight_level = level;
    if (s_board && s_board->spi_display.backlight >= 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}

uint8_t display_get_backlight(void) { return s_backlight_level; }

void display_flush_area(int x1, int y1, int x2, int y2, const uint16_t* buf) {
    if (!initialized || !buf)
        return;

    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    if (w <= 0 || h <= 0)
        return;

    /* On shared SPI buses, hold the bus mutex for the entire flush to prevent
     * radio SPI commands from interleaving between display command sequences.
     * This matches the proven Bramble approach: display and radio take
     * clean, non-overlapping turns with the SPI bus. */
    if (g_spi_mutex)
        xSemaphoreTake(g_spi_mutex, portMAX_DELAY);

    /* Set column address */
    st7789_write_cmd(0x2A); /* CASET */
    uint8_t ca[4] = {x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF};
    st7789_write_data(ca, 4);

    /* Set row address */
    st7789_write_cmd(0x2B); /* RASET */
    uint8_t ra[4] = {y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF};
    st7789_write_data(ra, 4);

    /* Write pixels: must copy through internal RAM because PSRAM is not
     * DMA-accessible on ESP32-S3 and LVGL buffers live in PSRAM. */
    st7789_write_cmd(0x2C); /* RAMWR */
    const uint8_t* src = (const uint8_t*)buf;
    size_t total = (size_t)w * h * 2;
    uint8_t dma_buf[512]; /* Stack: guaranteed internal SRAM */

    st7789_dc_data();
    for (size_t sent = 0; sent < total; sent += sizeof(dma_buf)) {
        size_t chunk = (total - sent > sizeof(dma_buf)) ? sizeof(dma_buf) : (total - sent);
        /* Copy and byte-swap: LVGL outputs little-endian RGB565 but ST7789
         * expects big-endian (MSB first). Swap each pixel's two bytes. */
        const uint16_t* src16 = (const uint16_t*)(src + sent);
        uint16_t* dst16 = (uint16_t*)dma_buf;
        size_t px_count = chunk / 2;
        for (size_t p = 0; p < px_count; p++) {
            dst16[p] = __builtin_bswap16(src16[p]);
        }
        spi_transaction_t t = {
            .length = chunk * 8,
            .tx_buffer = dma_buf,
        };
        esp_err_t ret = spi_device_transmit(spi, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI area flush error at offset %zu: %s", sent, esp_err_to_name(ret));
            break;
        }
    }

    if (g_spi_mutex)
        xSemaphoreGive(g_spi_mutex);
}

void display_set_rotated_180(bool rotated) {
    /* Task 1 compatibility shim: track state only, no ST7789 behavior change. */
    s_rotated_180 = rotated;
}

bool display_get_rotated_180(void) { return s_rotated_180; }
