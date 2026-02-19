#include "ui_graphics.h"
#include "lv_port_display.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "ui_gfx";

int ui_graphics_init(void) {
    ESP_LOGI(TAG, "Initializing LVGL graphical UI");
    lv_init();
    
    lv_display_t *disp = lv_port_display_init();
    if (!disp) {
        ESP_LOGE(TAG, "Display port init failed");
        return -1;
    }
    
    /* Temporary test — will be replaced by real UI */
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "BRAMBLE LVGL");
    lv_obj_center(label);
    
    ESP_LOGI(TAG, "LVGL initialized with display");
    return 0;
}

uint32_t ui_graphics_tick(void) {
    return lv_timer_handler();
}

void ui_graphics_notify(uint32_t event_mask) {
    (void)event_mask;
}
