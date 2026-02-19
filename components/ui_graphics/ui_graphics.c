#include "ui_graphics.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "ui_gfx";

int ui_graphics_init(void) {
    ESP_LOGI(TAG, "Initializing LVGL graphical UI");
    lv_init();
    ESP_LOGI(TAG, "LVGL initialized");
    return 0;
}

uint32_t ui_graphics_tick(void) {
    return lv_timer_handler();
}

void ui_graphics_notify(uint32_t event_mask) {
    (void)event_mask;
}
