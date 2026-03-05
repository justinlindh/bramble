/**
 * Simple button driver with board-specific GPIO configuration.
 * Polling-based with debounce, long press, and double press detection.
 */

#include "include/button.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char* TAG = "button";

static bool last_pressed = false;
static uint32_t press_start_ms = 0;
static uint32_t last_release_ms = 0;
static bool waiting_double = false;
static const bramble_board_config_t* s_board = NULL;

void button_init(void) {
    s_board = board_get_config();

    /* If board has no button GPIO, this is a no-op */
    if (s_board->button_gpio == -1) {
        ESP_LOGI(TAG, "Board has no button support");
        return;
    }

    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << s_board->button_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);
    ESP_LOGI(TAG, "Button initialized on GPIO%d", s_board->button_gpio);
}

ui_button_t button_poll(uint32_t now_ms) {
    /* If no button configured, always return none */
    if (!s_board || s_board->button_gpio == -1) {
        return BTN_NONE;
    }

    bool pressed = (gpio_get_level(s_board->button_gpio) == 0); /* active low */

    /* Rising edge: button just pressed */
    if (pressed && !last_pressed) {
        last_pressed = true;
        press_start_ms = now_ms;
        return BTN_NONE;
    }

    /* Falling edge: button just released */
    if (!pressed && last_pressed) {
        last_pressed = false;
        uint32_t hold_ms = now_ms - press_start_ms;

        /* Debounce */
        if (hold_ms < BUTTON_DEBOUNCE_MS) {
            return BTN_NONE;
        }

        /* Long press */
        if (hold_ms >= BUTTON_LONG_PRESS_MS) {
            waiting_double = false;
            return BTN_LONG_PRESS;
        }

        /* Short press — might be first of a double press */
        if (waiting_double) {
            /* Second press within gap → double press */
            waiting_double = false;
            return BTN_DOUBLE_PRESS;
        }

        /* First short press — wait for potential second */
        waiting_double = true;
        last_release_ms = now_ms;
        return BTN_NONE;
    }

    /* Check double-press timeout */
    if (waiting_double && !pressed && (now_ms - last_release_ms) > BUTTON_DOUBLE_GAP_MS) {
        waiting_double = false;
        return BTN_SHORT_PRESS;
    }

    return BTN_NONE;
}
