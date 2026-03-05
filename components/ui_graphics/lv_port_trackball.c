#include "lv_port_trackball.h"
#include "trackball.h"
#include "ui.h"
#include "esp_log.h"

static void trackball_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    ui_button_t btn = trackball_poll();

    data->state = LV_INDEV_STATE_RELEASED;
    data->enc_diff = 0;

    switch (btn) {
    case BTN_UP:
    case BTN_LEFT:
        data->enc_diff = -1;
        break;
    case BTN_DOWN:
    case BTN_RIGHT:
        data->enc_diff = 1;
        break;
    case BTN_SELECT:
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = LV_KEY_ENTER;
        break;
    default:
        break;
    }
}

lv_indev_t* lv_port_trackball_init(void) {
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev, trackball_read_cb);

    lv_group_t* g = lv_group_create();
    lv_group_set_default(g);
    lv_indev_set_group(indev, g);

    ESP_LOGI("lv_port_tb", "Trackball encoder input registered");
    return indev;
}
