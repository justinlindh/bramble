/**
 * SSD1306 128x64 OLED driver with board-specific I2C configuration
 * Uses ESP-IDF I2C master driver.
 */

#include "include/display.h"
#include "include/font_6x8.h"
#include "board_config.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "display";
static const bramble_board_config_t *s_board = NULL;

/* ── Framebuffer ─────────────────────────────────────────────────────── */

/* SSD1306 stores pixels in pages of 8 rows. 128 columns × 8 pages = 1024 bytes */
static uint8_t fb[DISPLAY_WIDTH * (DISPLAY_HEIGHT / 8)];

/* I2C handles */
static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t dev_handle;
static bool initialized = false;

/* ── I2C helpers ─────────────────────────────────────────────────────── */

static int ssd1306_cmd(uint8_t cmd) {
    uint8_t buf[2] = { 0x00, cmd }; /* Co=0, D/C#=0 → command */
    return i2c_master_transmit(dev_handle, buf, 2, 100) == ESP_OK ? 0 : -1;
}

static int ssd1306_cmd2(uint8_t cmd, uint8_t val) {
    uint8_t buf[3] = { 0x00, cmd, val };
    return i2c_master_transmit(dev_handle, buf, 3, 100) == ESP_OK ? 0 : -1;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int display_init(void) {
    /* Get board configuration */
    s_board = board_get_config();

    /* Only SSD1306 boards for now (I2C display) */
    if (!(s_board->capabilities & BOARD_CAP_DISPLAY_SSD1306)) {
        ESP_LOGW(TAG, "Display not supported on this board");
        return -1;
    }

    /* Enable Vext power if board has it (e.g., Heltec V3: GPIO36 LOW = power on) */
    if (s_board->i2c_display.vext != -1) {
        gpio_config_t vext_conf = {
            .pin_bit_mask = (1ULL << s_board->i2c_display.vext),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&vext_conf);
        gpio_set_level(s_board->i2c_display.vext, 0); /* LOW = power on */
        vTaskDelay(pdMS_TO_TICKS(50));       /* Let power stabilize */
        ESP_LOGI(TAG, "Vext power enabled (GPIO%d LOW)", s_board->i2c_display.vext);
    }

    /* Reset the display via RST pin */
    gpio_config_t rst_conf = {
        .pin_bit_mask = (1ULL << s_board->i2c_display.rst),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_conf);
    gpio_set_level(s_board->i2c_display.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(s_board->i2c_display.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Init I2C master bus */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = DISPLAY_I2C_PORT,
        .scl_io_num = s_board->i2c_display.scl,
        .sda_io_num = s_board->i2c_display.sda,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return -1;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_board->i2c_display.addr,
        .scl_speed_hz = DISPLAY_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* SSD1306 initialization sequence */
    ssd1306_cmd(0xAE);           /* Display OFF */
    ssd1306_cmd2(0xD5, 0x80);   /* Clock divide ratio */
    ssd1306_cmd2(0xA8, 0x3F);   /* Multiplex ratio: 64 */
    ssd1306_cmd2(0xD3, 0x00);   /* Display offset: 0 */
    ssd1306_cmd(0x40);           /* Start line: 0 */
    ssd1306_cmd2(0x8D, 0x14);   /* Charge pump: enable */
    ssd1306_cmd2(0x20, 0x00);   /* Memory addressing: horizontal */
    ssd1306_cmd(0xA1);           /* Segment remap: col 127 = SEG0 */
    ssd1306_cmd(0xC8);           /* COM scan direction: remapped */
    ssd1306_cmd2(0xDA, 0x12);   /* COM pins config: alternative */
    ssd1306_cmd2(0x81, 0xCF);   /* Contrast: 207 */
    ssd1306_cmd2(0xD9, 0xF1);   /* Pre-charge period */
    ssd1306_cmd2(0xDB, 0x40);   /* VCOMH deselect level */
    ssd1306_cmd(0xA4);           /* Display from RAM */
    ssd1306_cmd(0xA6);           /* Normal (not inverted) */
    ssd1306_cmd(0xAF);           /* Display ON */

    display_clear();
    display_flush();

    initialized = true;
    ESP_LOGI(TAG, "SSD1306 128x64 OLED initialized");
    return 0;
}

void display_clear(void) {
    memset(fb, 0, sizeof(fb));
}

void display_fill(void) {
    memset(fb, 0xFF, sizeof(fb));
}

void display_pixel(int x, int y, bool on) {
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) return;
    int page = y / 8;
    int bit = y % 8;
    if (on)
        fb[page * DISPLAY_WIDTH + x] |= (1 << bit);
    else
        fb[page * DISPLAY_WIDTH + x] &= ~(1 << bit);
}

void display_hline(int x, int y, int w) {
    for (int i = 0; i < w; i++)
        display_pixel(x + i, y, true);
}

void display_draw_text(int x, int y, const char *text) {
    while (*text) {
        uint8_t c = (uint8_t)*text;
        if (c >= 0x20 && c <= 0x7E) {
            const uint8_t *glyph = font6x8[c - 0x20];
            for (int col = 0; col < 6; col++) {
                uint8_t bits = glyph[col];
                for (int row = 0; row < 8; row++) {
                    if (bits & (1 << row))
                        display_pixel(x + col, y + row, true);
                }
            }
        }
        x += 6;
        if (x >= DISPLAY_WIDTH) break;
        text++;
    }
}

void display_draw_text_large(int x, int y, const char *text) {
    while (*text) {
        uint8_t c = (uint8_t)*text;
        if (c >= 0x20 && c <= 0x7E) {
            const uint8_t *glyph = font6x8[c - 0x20];
            for (int col = 0; col < 6; col++) {
                uint8_t bits = glyph[col];
                for (int row = 0; row < 8; row++) {
                    if (bits & (1 << row)) {
                        /* 2x scale: each pixel becomes a 2x2 block */
                        display_pixel(x + col * 2,     y + row * 2,     true);
                        display_pixel(x + col * 2 + 1, y + row * 2,     true);
                        display_pixel(x + col * 2,     y + row * 2 + 1, true);
                        display_pixel(x + col * 2 + 1, y + row * 2 + 1, true);
                    }
                }
            }
        }
        x += 12;
        if (x >= DISPLAY_WIDTH) break;
        text++;
    }
}

void display_flush(void) {
    if (!initialized || !dev_handle) return;

    /* Set column address range: 0 to 127 */
    uint8_t col_cmd[] = { 0x00, 0x21, 0x00, 0x7F };
    esp_err_t err = i2c_master_transmit(dev_handle, col_cmd, 4, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display_flush col_cmd failed: %d", err);
        return;
    }

    /* Set page address range: 0 to 7 */
    uint8_t page_cmd[] = { 0x00, 0x22, 0x00, 0x07 };
    err = i2c_master_transmit(dev_handle, page_cmd, 4, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display_flush page_cmd failed: %d", err);
        return;
    }

    /* Send framebuffer in chunks (I2C buffer limit) */
    for (int i = 0; i < (int)sizeof(fb); i += 128) {
        uint8_t buf[129];
        buf[0] = 0x40; /* Co=0, D/C#=1 → data */
        memcpy(buf + 1, fb + i, 128);
        err = i2c_master_transmit(dev_handle, buf, 129, 100);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "display_flush data chunk %d failed: %d", i / 128, err);
            return;
        }
    }
}

void display_power(bool on) {
    ssd1306_cmd(on ? 0xAF : 0xAE);
}

void display_set_contrast(uint8_t val) {
    ssd1306_cmd2(0x81, val);
}

void display_invert(bool invert) {
    ssd1306_cmd(invert ? 0xA7 : 0xA6);
}

void display_set_backlight(uint8_t level) {
    (void)level;
    /* SSD1306 boards don't expose a backlight PWM path. */
}
