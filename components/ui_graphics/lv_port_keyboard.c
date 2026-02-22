#include "lv_port_keyboard.h"
#include "keyboard.h"
#include "sleep_manager.h"
#include "esp_log.h"

static uint32_t last_key = 0;

static void keyboard_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    
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
        /* Signal activity to reset sleep timer / wake display */
        sleep_manager_activity();
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    
    data->key = last_key;
}

lv_indev_t *lv_port_keyboard_init(void) {
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, keyboard_read_cb);

    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_indev_set_group(indev, g);
    }

    ESP_LOGI("lv_port_kb", "Keyboard input device registered");
    return indev;
}
