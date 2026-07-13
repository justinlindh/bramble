#include "lv_port_trackball.h"
#include "trackball.h"
#include "ui.h"
#include "ui_zone.h"
#include "sleep_manager.h"
#include "esp_log.h"

/* The trackball is a KEYPAD indev (not an encoder): only a keypad can carry
 * the four directions as distinct keys, which the zone navigator needs to map
 * UP/DOWN and LEFT/RIGHT onto different axes. ui_zone_translate() turns each
 * physical detent into the LVGL key to emit (or performs a zone hop and
 * returns 0). Like the keyboard port, every delivered key needs an explicit
 * release tick after it so LVGL sees a fresh RELEASED->PRESSED edge for the
 * next one; back-to-back injected detents would otherwise read as one held
 * key and only the first would register. */
static uint32_t s_last_key = 0;
static bool s_release_pending = false;

static void trackball_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;

    if (s_release_pending) {
        s_release_pending = false;
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = s_last_key;
        return;
    }

    ui_button_t btn = trackball_poll();
    if (btn == BTN_NONE) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = s_last_key;
        return;
    }

    sleep_manager_activity();

    uint32_t key = ui_zone_translate(btn);
    if (key == 0) {
        /* Pure zone hop: the indev group was already switched inside
         * translate; emit nothing this tick. */
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = 0;
        return;
    }

    s_last_key = key;
    data->key = key;
    data->state = LV_INDEV_STATE_PRESSED;
    s_release_pending = true; /* next read emits the release edge */
}

lv_indev_t* lv_port_trackball_init(void) {
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, trackball_read_cb);

    /* Start on the content zone (ui_zone_init made it the default group). */
    lv_indev_set_group(indev, lv_group_get_default());

    ESP_LOGI("lv_port_tb", "Trackball keypad input registered");
    return indev;
}
