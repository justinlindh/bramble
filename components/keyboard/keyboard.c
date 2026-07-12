/**
 * Keyboard driver for T-Deck Plus
 * ESP32-C3 sub-MCU keyboard at I2C address 0x55
 *
 * Pure polling: keyboard_poll() is called on the UI task and both produces
 * (buffer_push) and consumes (buffer_pop) key events on that one task, so the
 * circular buffer needs no locking today. If the GPIO46 data-ready line is
 * ever wired to an ISR that calls buffer_push(), key_head/key_tail become a
 * cross-context producer/consumer pair and their non-atomic ++ must then be
 * guarded (see trackball.c for the spinlock pattern).
 */

#include "include/keyboard.h"
#include "board_config.h"

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_keys.h"
#include <string.h>

static const char* TAG = "keyboard";
static const bramble_board_config_t* s_board = NULL;

/* I2C handles */
static i2c_master_bus_handle_t bus_handle = NULL;
static i2c_master_dev_handle_t dev_handle = NULL;
static bool initialized = false;

/* Circular buffer for key events */
#define KEY_BUFFER_SIZE 32
static char key_buffer[KEY_BUFFER_SIZE];
static volatile int key_head = 0;
static volatile int key_tail = 0;

/* Polling cooldown — avoid hammering I2C on every LVGL tick (~30ms) */
static int64_t last_poll_us = 0;
#define POLL_INTERVAL_US 20000 /* 20ms minimum between I2C reads */

/* Backlight persistence */
#define DEFAULT_BACKLIGHT 80 /* Sane default — not too bright */
#define NVS_NAMESPACE NVS_NS_BRAMBLE
#define NVS_KEY_BACKLIGHT "kb_backlight"
static uint8_t s_backlight_brightness = DEFAULT_BACKLIGHT;

/* ── NVS helpers ────────────────────────────────────────────────────── */

static void nvs_load_backlight(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        s_backlight_brightness = DEFAULT_BACKLIGHT;
        return;
    }
    uint8_t brightness = DEFAULT_BACKLIGHT;
    nvs_get_u8(h, NVS_KEY_BACKLIGHT, &brightness);
    nvs_close(h);
    s_backlight_brightness = (brightness > 100) ? 100 : brightness;
    ESP_LOGI(TAG, "Backlight brightness loaded: %u", s_backlight_brightness);
}

static void nvs_save_backlight(uint8_t brightness) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, NVS_KEY_BACKLIGHT, brightness);
    nvs_commit(h);
    nvs_close(h);
}

/* ── Circular Buffer Helpers ────────────────────────────────────────── */

static inline int next_index(int idx) { return (idx + 1) % KEY_BUFFER_SIZE; }

static inline bool buffer_full(void) { return next_index(key_head) == key_tail; }

static inline bool buffer_empty(void) { return key_head == key_tail; }

static inline void buffer_push(char c) {
    if (!buffer_full()) {
        key_buffer[key_head] = c;
        key_head = next_index(key_head);
    } else {
        ESP_LOGW(TAG, "Key buffer overflow, dropping key");
    }
}

static inline bool buffer_pop(char* out) {
    if (buffer_empty())
        return false;
    *out = key_buffer[key_tail];
    key_tail = next_index(key_tail);
    return true;
}

/* ── I2C Read Key ───────────────────────────────────────────────────── */

/* Read one byte from keyboard MCU. Non-zero = key code, 0 = no key.
 * Called directly from keyboard_poll() in polling mode (no ISR). */
static void keyboard_read_key(void) {
    uint8_t key = 0;
    esp_err_t ret = i2c_master_receive(dev_handle, &key, 1, 50);

    if (ret != ESP_OK) {
        /* Suppress repeated noise — only log every ~2 sec */
        static int64_t last_err_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - last_err_us > 2000000) {
            ESP_LOGW(TAG, "I2C read failed: %s", esp_err_to_name(ret));
            last_err_us = now;
        }
    } else if (key != 0) {
        ESP_LOGI(TAG, "Key: '%c' (0x%02x)", (key >= 0x20 && key < 0x7F) ? key : '?', key);
        buffer_push(key);
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

int keyboard_init(void) {
    s_board = board_get_config();

    /* Only T-Deck Plus has keyboard */
    if (!(s_board->capabilities & BOARD_CAP_KEYBOARD)) {
        ESP_LOGD(TAG, "Keyboard not supported on this board");
        return -1;
    }

    if (s_board->i2c_sda == -1 || s_board->i2c_scl == -1) {
        ESP_LOGE(TAG, "I2C pins not configured");
        return -1;
    }

    /* Initialize I2C bus */
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = s_board->i2c_scl,
        .sda_io_num = s_board->i2c_sda,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(ret));
        return -1;
    }

    /* Add keyboard device */
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x55,
        .scl_speed_hz = 100000, /* 100 kHz */
    };

    ret = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add keyboard I2C device: %s", esp_err_to_name(ret));
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
        return -1;
    }

    /* GPIO 46 is the keyboard data-ready line on T-Deck Plus, but it is
     * unreliable as a hardware interrupt (strapping pin behaviour, pull
     * direction, board-level noise).  Bramble likewise leaves it unused.
     * We use pure I2C polling instead: keyboard_poll() reads one byte every
     * POLL_INTERVAL_US microseconds; the MCU returns 0x00 when idle. */
    ESP_LOGD(TAG, "Keyboard running in polling mode (GPIO%d ISR skipped)", s_board->keyboard_int);

    initialized = true;

    /* Load persisted backlight value and apply it */
    nvs_load_backlight();
    uint8_t brightness_hw = (uint8_t)(s_backlight_brightness * 255 / 100);
    uint8_t cmd[2] = {0x01, brightness_hw};
    i2c_master_transmit(dev_handle, cmd, sizeof(cmd), 100);

    ESP_LOGI(TAG, "Keyboard initialized (I2C 0x55, polling mode, backlight=%u%%)",
             s_backlight_brightness);
    return 0;
}

bool keyboard_poll(char* out) {
    if (!initialized || !out)
        return false;

    /* Rate-limit I2C reads so we don't saturate the bus.
     * LVGL calls this every ~30ms; we read every 20ms max. */
    int64_t now = esp_timer_get_time();
    if (now - last_poll_us >= POLL_INTERVAL_US) {
        last_poll_us = now;
        keyboard_read_key();
    }

    /* Return any buffered key */
    return buffer_pop(out);
}

bool keyboard_has_data(void) {
    if (!initialized)
        return false;
    return !buffer_empty();
}

i2c_master_bus_handle_t keyboard_get_i2c_bus(void) { return bus_handle; }

void keyboard_set_backlight(uint8_t brightness) {
    if (!initialized || !dev_handle)
        return;
    /* I2C command to keyboard MCU at 0x55:
     *   byte[0] = 0x01  (LILYGO_KB_BRIGHTNESS_CMD register)
     *   byte[1] = 0..255 (PWM duty; 0 = off, 255 = maximum brightness)
     * Note: the MCU may only implement on/off (treating any value >0 as on),
     * but sending the full range is safe and correct. */
    uint8_t cmd[2] = {0x01, brightness};
    esp_err_t ret = i2c_master_transmit(dev_handle, cmd, sizeof(cmd), 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Keyboard backlight set failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Keyboard backlight brightness=%u", brightness);
    }
}

void keyboard_set_backlight_percent(uint8_t percent) {
    if (percent > 100)
        percent = 100;
    s_backlight_brightness = percent;
    nvs_save_backlight(percent);
    /* Map 0-100 → 0-255 for hardware */
    uint8_t brightness_hw = (uint8_t)(percent * 255 / 100);
    keyboard_set_backlight(brightness_hw);
    ESP_LOGI(TAG, "Keyboard backlight set to %u%%", percent);
}

uint8_t keyboard_get_backlight_percent(void) { return s_backlight_brightness; }

#else /* !CONFIG_BRAMBLE_BOARD_TDECK_PLUS */

/* Stub implementations for non-T-Deck boards */

int keyboard_init(void) { return -1; /* Not supported */ }

bool keyboard_poll(char* out) {
    (void)out;
    return false;
}

bool keyboard_has_data(void) { return false; }

void keyboard_set_backlight(uint8_t brightness) { (void)brightness; }

void keyboard_set_backlight_percent(uint8_t percent) { (void)percent; }

uint8_t keyboard_get_backlight_percent(void) { return 0; }

#endif /* CONFIG_BRAMBLE_BOARD_TDECK_PLUS */
