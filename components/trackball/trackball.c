/**
 * Trackball driver for T-Deck Plus
 * Hall effect sensor trackball with interrupt-driven event counters
 */

#include "include/trackball.h"
#include "board_config.h"

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS

#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "trackball";
static const bramble_board_config_t *s_board = NULL;
static bool initialized = false;

/* Event counters (incremented by ISRs) */
static volatile int count_up = 0;
static volatile int count_down = 0;
static volatile int count_left = 0;
static volatile int count_right = 0;
static volatile int count_center = 0;

/* ── ISR Handlers ───────────────────────────────────────────────────── */

static void IRAM_ATTR trackball_up_isr(void *arg) {
    count_up++;
}

static void IRAM_ATTR trackball_down_isr(void *arg) {
    count_down++;
}

static void IRAM_ATTR trackball_left_isr(void *arg) {
    count_left++;
}

static void IRAM_ATTR trackball_right_isr(void *arg) {
    count_right++;
}

static void IRAM_ATTR trackball_center_isr(void *arg) {
    count_center++;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int trackball_init(void) {
    s_board = board_get_config();

    /* Only T-Deck Plus has trackball */
    if (!(s_board->capabilities & BOARD_CAP_TRACKBALL)) {
        ESP_LOGD(TAG, "Trackball not supported on this board");
        return -1;
    }

    /* Validate pins */
    if (s_board->trackball.up == -1 || s_board->trackball.down == -1 ||
        s_board->trackball.left == -1 || s_board->trackball.right == -1 ||
        s_board->trackball.center == -1) {
        ESP_LOGE(TAG, "Trackball pins not configured");
        return -1;
    }

    /* Configure all trackball GPIOs with NEGEDGE interrupts */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_board->trackball.up) |
                        (1ULL << s_board->trackball.down) |
                        (1ULL << s_board->trackball.left) |
                        (1ULL << s_board->trackball.right) |
                        (1ULL << s_board->trackball.center),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io_conf);

    /* Install ISR service if not already installed */
    gpio_install_isr_service(0);

    /* Add ISR handlers */
    gpio_isr_handler_add(s_board->trackball.up, trackball_up_isr, NULL);
    gpio_isr_handler_add(s_board->trackball.down, trackball_down_isr, NULL);
    gpio_isr_handler_add(s_board->trackball.left, trackball_left_isr, NULL);
    gpio_isr_handler_add(s_board->trackball.right, trackball_right_isr, NULL);
    gpio_isr_handler_add(s_board->trackball.center, trackball_center_isr, NULL);

    initialized = true;
    ESP_LOGI(TAG, "Trackball initialized");
    return 0;
}

ui_button_t trackball_poll(void) {
    if (!initialized) return BTN_NONE;

    /* Priority: center > up > down > left > right */
    if (count_center > 0) {
        count_center--;
        return BTN_SELECT;
    }
    if (count_up > 0) {
        count_up--;
        return BTN_UP;
    }
    if (count_down > 0) {
        count_down--;
        return BTN_DOWN;
    }
    if (count_left > 0) {
        count_left--;
        return BTN_LEFT;
    }
    if (count_right > 0) {
        count_right--;
        return BTN_RIGHT;
    }

    return BTN_NONE;
}

#else  /* !CONFIG_BRAMBLE_BOARD_TDECK_PLUS */

/* Stub implementations for non-T-Deck boards */

int trackball_init(void) {
    return -1;  /* Not supported */
}

ui_button_t trackball_poll(void) {
    return BTN_NONE;
}

#endif /* CONFIG_BRAMBLE_BOARD_TDECK_PLUS */
