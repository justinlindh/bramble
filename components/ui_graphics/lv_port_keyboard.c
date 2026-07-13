#include "lv_port_keyboard.h"
#include "keyboard.h"
#include "sleep_manager.h"
#include "esp_log.h"

static uint32_t last_key = 0;
static bool release_pending = false;

static void keyboard_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;

    /* LVGL's keypad indev latches a key only on the RELEASED->PRESSED edge, so
     * every key needs a release between it and the next one. Real typing gets
     * that for free (the I2C poll is slower than LVGL's read, so idle reads fall
     * through to the RELEASED branch below), but injected keys (the bench debug
     * RPC, keyboard_inject_char) drain back-to-back from their ring and would
     * otherwise present as one continuous press, swallowing every char after the
     * first. Emit an explicit release tick after each delivered key so a
     * multi-char inject types in full. Harmless for real keys: the driver already
     * hands us discrete buffered chars, not a held-key state. */
    if (release_pending) {
        release_pending = false;
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = last_key;
        return;
    }

    char ch;
    if (keyboard_poll(&ch)) {
        if (ch == '\n' || ch == '\r') {
            last_key = LV_KEY_ENTER;
        } else if (ch == '\b' || ch == 127) {
            last_key = LV_KEY_BACKSPACE;
        } else if (ch == 27) {
            last_key = LV_KEY_ESC;
        } else if (ch == '\t') {
            last_key = LV_KEY_NEXT;
        } else {
            last_key = ch;
        }
        data->state = LV_INDEV_STATE_PRESSED;
        release_pending = true; /* next read emits the release edge for this key */
        /* Signal activity to reset sleep timer / wake display */
        sleep_manager_activity();
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }

    data->key = last_key;
}

lv_indev_t* lv_port_keyboard_init(void) {
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, keyboard_read_cb);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_indev_set_group(indev, g);
    }

    ESP_LOGI("lv_port_kb", "Keyboard input device registered");
    return indev;
}
