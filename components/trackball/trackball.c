/**
 * Trackball driver for T-Deck Plus
 * Hall effect sensor trackball with interrupt-driven event counters
 */

#include "trackball.h"
#include "board_config.h"

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char* TAG = "trackball";
static const bramble_board_config_t* s_board = NULL;
static bool initialized = false;

/* Event counters (incremented by ISRs, decremented by trackball_poll).
 * `volatile int` does NOT make ++/-- atomic, so ISR and poll are serialized
 * with a spinlock to avoid a lost increment (a dropped detent/select). */
static portMUX_TYPE s_tb_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int count_up = 0;
static volatile int count_down = 0;
static volatile int count_left = 0;
static volatile int count_right = 0;
static volatile int count_center = 0;

/* ── ISR Handler ────────────────────────────────────────────────────── */

/* `gpio_isr_handler_add` passes the per-pin `arg` registered below, which
 * points at that direction's counter, so one handler serves all five pins. */
static void IRAM_ATTR trackball_isr(void* arg) {
    portENTER_CRITICAL_ISR(&s_tb_lock);
    (*(volatile int*)arg)++;
    portEXIT_CRITICAL_ISR(&s_tb_lock);
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
        .pin_bit_mask = (1ULL << s_board->trackball.up) | (1ULL << s_board->trackball.down) |
                        (1ULL << s_board->trackball.left) | (1ULL << s_board->trackball.right) |
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
    gpio_isr_handler_add(s_board->trackball.up, trackball_isr, (void*)&count_up);
    gpio_isr_handler_add(s_board->trackball.down, trackball_isr, (void*)&count_down);
    gpio_isr_handler_add(s_board->trackball.left, trackball_isr, (void*)&count_left);
    gpio_isr_handler_add(s_board->trackball.right, trackball_isr, (void*)&count_right);
    gpio_isr_handler_add(s_board->trackball.center, trackball_isr, (void*)&count_center);

    initialized = true;
    ESP_LOGI(TAG, "Trackball initialized");
    return 0;
}

ui_button_t trackball_poll(void) {
    if (!initialized)
        return BTN_NONE;

    /* Priority: center > up > down > left > right.
     * Decrement under the spinlock so an ISR increment cannot be lost in the
     * read-modify-write. */
    ui_button_t btn = BTN_NONE;
    portENTER_CRITICAL(&s_tb_lock);
    if (count_center > 0) {
        count_center--;
        btn = BTN_SELECT;
    } else if (count_up > 0) {
        count_up--;
        btn = BTN_UP;
    } else if (count_down > 0) {
        count_down--;
        btn = BTN_DOWN;
    } else if (count_left > 0) {
        count_left--;
        btn = BTN_LEFT;
    } else if (count_right > 0) {
        count_right--;
        btn = BTN_RIGHT;
    }
    portEXIT_CRITICAL(&s_tb_lock);

    return btn;
}

bool trackball_inject(ui_button_t btn) {
    if (!initialized)
        return false;

    /* Feed the same counters the ISRs feed, under the same spinlock, so an
     * injected event is indistinguishable from a real one by the time
     * trackball_poll() drains it. */
    portENTER_CRITICAL(&s_tb_lock);
    bool ok = true;
    switch (btn) {
    case BTN_UP:
        count_up++;
        break;
    case BTN_DOWN:
        count_down++;
        break;
    case BTN_LEFT:
        count_left++;
        break;
    case BTN_RIGHT:
        count_right++;
        break;
    case BTN_SELECT:
        count_center++;
        break;
    default:
        ok = false;
        break;
    }
    portEXIT_CRITICAL(&s_tb_lock);
    return ok;
}

#else /* !CONFIG_BRAMBLE_BOARD_TDECK_PLUS */

/* Stub implementations for non-T-Deck boards */

int trackball_init(void) { return -1; /* Not supported */ }

ui_button_t trackball_poll(void) { return BTN_NONE; }

bool trackball_inject(ui_button_t btn) {
    (void)btn;
    return false;
}

#endif /* CONFIG_BRAMBLE_BOARD_TDECK_PLUS */
