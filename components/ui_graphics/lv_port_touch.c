#include "lv_port_touch.h"
#include "touch.h"
#include "sleep_manager.h"
#include "esp_log.h"

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    touch_point_t pt;
    if (touch_read(&pt) && pt.pressed) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PRESSED;
        /* Signal activity to reset sleep timer / wake display */
        sleep_manager_activity();
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

lv_indev_t *lv_port_touch_init(void) {
    if (touch_init() != 0) {
        ESP_LOGW("lv_port_touch", "Touch init failed - touch disabled");
        return NULL;
    }

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    ESP_LOGI("lv_port_touch", "Touch input device registered");
    return indev;
}
