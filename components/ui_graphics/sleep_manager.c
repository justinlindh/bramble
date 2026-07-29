/**
 * Sleep manager for T-Deck Plus
 * Automatically turns off display after inactivity timeout
 * Continues to receive messages while asleep (mesh still active)
 */

#include "sleep_manager.h"
#include "display.h"
#include "keyboard.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_keys.h"

static const char* TAG = "sleep_mgr";

/* NVS persistence */
#define NVS_NAMESPACE NVS_NS_BRAMBLE
#define NVS_KEY_SLEEP_EN "sleep_enabled"
#define NVS_KEY_SLEEP_TOUT "sleep_timeout"

/* Default values */
#define DEFAULT_TIMEOUT_SEC 60 /* 1 minute */

/* Sleep state.
 *
 * Threading model: all display-power / backlight mutation and the `asleep`
 * and saved-backlight fields are owned by the UI task. The esp_timer callback
 * (`sleep_timer_cb`, runs in the esp_timer service task) does NO blocking SPI:
 * it only decides the timeout has elapsed and raises `want_sleep`. The UI task
 * drains that flag in `sleep_manager_process()` and performs the actual panel
 * power-down. `sleep_manager_activity()` (the wake path) is likewise only ever
 * called on the UI task. The two scalar fields shared across tasks
 * (`last_activity_us`, `want_sleep`) are guarded by s_lock. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static struct {
    bool initialized;
    bool enabled;
    bool asleep; /* UI-task owned */
    bool want_sleep;
    uint16_t timeout_sec;
    int64_t last_activity_us;
    esp_timer_handle_t timer;
    /* Saved backlight levels for wake restore (UI-task owned) */
    uint8_t saved_kbd_backlight;
    uint8_t saved_disp_backlight;
} s_sleep = {
    .initialized = false,
    .enabled = true,
    .asleep = false,
    .want_sleep = false,
    .timeout_sec = DEFAULT_TIMEOUT_SEC,
    .last_activity_us = 0,
    .timer = NULL,
    .saved_kbd_backlight = 0,
    .saved_disp_backlight = 0,
};

/* ── NVS helpers ────────────────────────────────────────────────────── */

static void nvs_load_prefs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        s_sleep.enabled = true;
        s_sleep.timeout_sec = DEFAULT_TIMEOUT_SEC;
        return;
    }
    uint8_t enabled = 1;
    uint16_t timeout = DEFAULT_TIMEOUT_SEC;
    nvs_get_u8(h, NVS_KEY_SLEEP_EN, &enabled);
    nvs_get_u16(h, NVS_KEY_SLEEP_TOUT, &timeout);
    nvs_close(h);
    s_sleep.enabled = (enabled != 0);
    s_sleep.timeout_sec = (timeout > 0 && timeout <= 3600) ? timeout : DEFAULT_TIMEOUT_SEC;
    ESP_LOGI(TAG, "Sleep prefs loaded: enabled=%d timeout=%us", s_sleep.enabled,
             s_sleep.timeout_sec);
}

static void nvs_save_enabled(bool enabled) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed for sleep enabled: %d", err);
        return;
    }
    err = nvs_set_u8(h, NVS_KEY_SLEEP_EN, (uint8_t)enabled);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "failed to persist sleep enabled: %d", err);
}

static void nvs_save_timeout(uint16_t timeout_sec) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed for sleep timeout: %d", err);
        return;
    }
    err = nvs_set_u16(h, NVS_KEY_SLEEP_TOUT, timeout_sec);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "failed to persist sleep timeout: %d", err);
}

/* ── Timer callback ─────────────────────────────────────────────────── */

/* Runs in the esp_timer service task. MUST NOT do blocking SPI here: a
 * display_power() over the shared bus would stall every other esp_timer
 * callback, including the 1 ms lv_tick. So this only detects the timeout and
 * raises want_sleep; sleep_manager_process() (UI task) does the panel work. */
static void sleep_timer_cb(void* arg) {
    (void)arg;
    if (!s_sleep.enabled)
        return;

    int64_t now = esp_timer_get_time();
    int64_t timeout_us = (int64_t)s_sleep.timeout_sec * 1000000;

    portENTER_CRITICAL(&s_lock);
    int64_t elapsed_us = now - s_sleep.last_activity_us;
    if (!s_sleep.asleep && elapsed_us >= timeout_us) {
        s_sleep.want_sleep = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

/* ── Public API ─────────────────────────────────────────────────────── */

int sleep_manager_init(void) {
    if (s_sleep.initialized) {
        ESP_LOGW(TAG, "Sleep manager already initialized");
        return 0;
    }

    /* Load persisted preferences */
    nvs_load_prefs();

    /* Create periodic timer (checks every second) */
    esp_timer_create_args_t timer_args = {
        .callback = sleep_timer_cb,
        .name = "sleep_check",
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_sleep.timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(err));
        return -1;
    }

    /* Start timer with 1-second period */
    err = esp_timer_start_periodic(s_sleep.timer, 1000000); /* 1 second */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(err));
        esp_timer_delete(s_sleep.timer);
        s_sleep.timer = NULL;
        return -1;
    }

    s_sleep.last_activity_us = esp_timer_get_time();
    s_sleep.asleep = false;
    s_sleep.initialized = true;

    ESP_LOGI(TAG, "Sleep manager initialized (enabled=%d, timeout=%us)", s_sleep.enabled,
             s_sleep.timeout_sec);
    return 0;
}

/* UI-task side of the sleep transition. Drains the want_sleep flag raised by
 * the esp_timer callback and, only if the panel is still genuinely idle,
 * powers it down. Doing the SPI here (not in the timer cb) keeps blocking bus
 * work off the esp_timer service task. Call periodically from the UI task. */
void sleep_manager_process(void) {
    if (!s_sleep.initialized)
        return;

    bool do_sleep = false;
    portENTER_CRITICAL(&s_lock);
    if (s_sleep.want_sleep) {
        s_sleep.want_sleep = false;
        /* Atomic re-check: a keypress landing at the timeout boundary updates
         * last_activity_us; if it did, abandon the pending sleep so we don't
         * blank the panel right after the user interacted. */
        int64_t elapsed_us = esp_timer_get_time() - s_sleep.last_activity_us;
        int64_t timeout_us = (int64_t)s_sleep.timeout_sec * 1000000;
        if (s_sleep.enabled && !s_sleep.asleep && elapsed_us >= timeout_us) {
            do_sleep = true;
            s_sleep.asleep = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (do_sleep) {
        ESP_LOGI(TAG, "Entering sleep mode (timeout=%us)", s_sleep.timeout_sec);

        /* Save current backlight levels before turning off */
        s_sleep.saved_kbd_backlight = keyboard_get_backlight_percent();
        s_sleep.saved_disp_backlight = display_get_backlight();

        /* Turn off display panel and both backlights */
        display_power(false);
        display_set_backlight(0);
        keyboard_set_backlight(0); /* Raw 0-255 value */
    }
}

void sleep_manager_activity(void) {
    if (!s_sleep.initialized)
        return;

    portENTER_CRITICAL(&s_lock);
    s_sleep.last_activity_us = esp_timer_get_time();
    /* Any activity cancels a sleep the timer may have just requested. */
    s_sleep.want_sleep = false;
    portEXIT_CRITICAL(&s_lock);

    /* Wake up if asleep */
    if (s_sleep.asleep) {
        ESP_LOGI(TAG, "Waking from sleep (restoring backlights: kbd=%u%%, disp=%u)",
                 s_sleep.saved_kbd_backlight, s_sleep.saved_disp_backlight);

        /* Turn display panel back on */
        display_power(true);

        /* Restore saved backlight levels */
        display_set_backlight(s_sleep.saved_disp_backlight);
        uint8_t kbd_hw = (uint8_t)(s_sleep.saved_kbd_backlight * 255 / 100);
        keyboard_set_backlight(kbd_hw);

        s_sleep.asleep = false;
    }
}

void sleep_manager_set_enabled(bool enabled) {
    s_sleep.enabled = enabled;
    nvs_save_enabled(enabled);
    ESP_LOGI(TAG, "Sleep mode %s", enabled ? "enabled" : "disabled");

    /* If disabling and currently asleep, wake up and restore backlights */
    if (!enabled && s_sleep.asleep) {
        ESP_LOGI(TAG, "Waking from sleep (sleep disabled)");
        display_power(true);
        display_set_backlight(s_sleep.saved_disp_backlight);
        uint8_t kbd_hw = (uint8_t)(s_sleep.saved_kbd_backlight * 255 / 100);
        keyboard_set_backlight(kbd_hw);
        s_sleep.asleep = false;
    }
}

bool sleep_manager_get_enabled(void) { return s_sleep.enabled; }

void sleep_manager_set_timeout(uint16_t seconds) {
    if (seconds == 0 || seconds > 3600) {
        ESP_LOGW(TAG, "Invalid timeout %u, keeping %u", seconds, s_sleep.timeout_sec);
        return;
    }
    s_sleep.timeout_sec = seconds;
    nvs_save_timeout(seconds);
    ESP_LOGI(TAG, "Sleep timeout set to %us", seconds);
}

uint16_t sleep_manager_get_timeout(void) { return s_sleep.timeout_sec; }
