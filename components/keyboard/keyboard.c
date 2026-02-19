/**
 * Keyboard driver for T-Deck Plus
 * ESP32-C3 sub-MCU keyboard at I2C address 0x55
 * Interrupt-driven with circular buffer
 */

#include "include/keyboard.h"
#include "board_config.h"

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "keyboard";
static const bramble_board_config_t *s_board = NULL;

/* I2C handles */
static i2c_master_bus_handle_t bus_handle = NULL;
static i2c_master_dev_handle_t dev_handle = NULL;
static bool initialized = false;

/* Circular buffer for key events */
#define KEY_BUFFER_SIZE 32
static char key_buffer[KEY_BUFFER_SIZE];
static volatile int key_head = 0;
static volatile int key_tail = 0;

/* Interrupt flag */
static volatile bool key_available = false;

/* ── Circular Buffer Helpers ────────────────────────────────────────── */

static inline int next_index(int idx) {
    return (idx + 1) % KEY_BUFFER_SIZE;
}

static inline bool buffer_full(void) {
    return next_index(key_head) == key_tail;
}

static inline bool buffer_empty(void) {
    return key_head == key_tail;
}

static inline void buffer_push(char c) {
    if (!buffer_full()) {
        key_buffer[key_head] = c;
        key_head = next_index(key_head);
    } else {
        ESP_LOGW(TAG, "Key buffer overflow, dropping key");
    }
}

static inline bool buffer_pop(char *out) {
    if (buffer_empty()) return false;
    *out = key_buffer[key_tail];
    key_tail = next_index(key_tail);
    return true;
}

/* ── ISR ────────────────────────────────────────────────────────────── */

static void IRAM_ATTR keyboard_isr_handler(void *arg) {
    key_available = true;
}

/* ── I2C Read Key ───────────────────────────────────────────────────── */

static void keyboard_read_key(void) {
    uint8_t key = 0;
    esp_err_t ret = i2c_master_receive(dev_handle, &key, 1, 100);
    
    if (ret == ESP_OK && key != 0) {
        buffer_push(key);
    }
    
    key_available = false;
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
        .scl_speed_hz = 100000,  /* 100 kHz */
    };

    ret = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add keyboard I2C device: %s", esp_err_to_name(ret));
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
        return -1;
    }

    /* Configure interrupt GPIO */
    if (s_board->keyboard_int != -1) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << s_board->keyboard_int),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&io_conf);

        /* Install ISR service if not already installed */
        gpio_install_isr_service(0);
        gpio_isr_handler_add(s_board->keyboard_int, keyboard_isr_handler, NULL);
    }

    initialized = true;
    ESP_LOGI(TAG, "Keyboard initialized (I2C 0x55)");
    return 0;
}

bool keyboard_poll(char *out) {
    if (!initialized || !out) return false;

    /* Check interrupt flag and read if available */
    if (key_available) {
        keyboard_read_key();
    }

    /* Pop from buffer */
    return buffer_pop(out);
}

bool keyboard_has_data(void) {
    if (!initialized) return false;
    return !buffer_empty();
}

i2c_master_bus_handle_t keyboard_get_i2c_bus(void) {
    return bus_handle;
}

#else  /* !CONFIG_BRAMBLE_BOARD_TDECK_PLUS */

/* Stub implementations for non-T-Deck boards */

int keyboard_init(void) {
    return -1;  /* Not supported */
}

bool keyboard_poll(char *out) {
    (void)out;
    return false;
}

bool keyboard_has_data(void) {
    return false;
}

#endif /* CONFIG_BRAMBLE_BOARD_TDECK_PLUS */
