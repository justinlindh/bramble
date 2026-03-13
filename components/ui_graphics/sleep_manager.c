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

/* Sleep state */
static struct {
    bool initialized;
    bool enabled;
    bool asleep;
    uint16_t timeout_sec;
    int64_t last_activity_us;
    esp_timer_handle_t timer;
    /* Saved backlight levels for wake restore */
    uint8_t saved_kbd_backlight;
    uint8_t saved_disp_backlight;
} s_sleep = {
    .initialized = false,
    .enabled = true,
    .asleep = false,
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
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, NVS_KEY_SLEEP_EN, (uint8_t)enabled);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_timeout(uint16_t timeout_sec) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u16(h, NVS_KEY_SLEEP_TOUT, timeout_sec);
    nvs_commit(h);
    nvs_close(h);
}

/* ── Timer callback ─────────────────────────────────────────────────── */

static void sleep_timer_cb(void* arg) {
    (void)arg;
    if (!s_sleep.enabled || s_sleep.asleep)
        return;

    int64_t now = esp_timer_get_time();
    int64_t elapsed_us = now - s_sleep.last_activity_us;
    int64_t timeout_us = (int64_t)s_sleep.timeout_sec * 1000000;

    if (elapsed_us >= timeout_us) {
        ESP_LOGI(TAG, "Entering sleep mode (timeout=%us)", s_sleep.timeout_sec);

        /* Save current backlight levels before turning off */
        s_sleep.saved_kbd_backlight = keyboard_get_backlight_percent();
        /* Note: display backlight is controlled by LEDC PWM but we don't have
         * a get_backlight() API yet. For now, we assume it's at max (255). */
        s_sleep.saved_disp_backlight = 255;

        /* Turn off display panel and both backlights */
        display_power(false);
        display_set_backlight(0);
        keyboard_set_backlight(0); /* Raw 0-255 value */

        s_sleep.asleep = true;
    }
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

void sleep_manager_deinit(void) {
    if (!s_sleep.initialized)
        return;

    if (s_sleep.timer) {
        esp_timer_stop(s_sleep.timer);
        esp_timer_delete(s_sleep.timer);
        s_sleep.timer = NULL;
    }

    s_sleep.initialized = false;
    ESP_LOGI(TAG, "Sleep manager deinitialized");
}

void sleep_manager_activity(void) {
    if (!s_sleep.initialized)
        return;

    s_sleep.last_activity_us = esp_timer_get_time();

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

bool sleep_manager_is_asleep(void) { return s_sleep.asleep; }

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
