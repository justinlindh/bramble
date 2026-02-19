#include "ui_graphics.h"
#include "lv_port_display.h"
#include "lv_port_touch.h"
#include "lv_port_trackball.h"
#include "lv_port_keyboard.h"
#include "theme/bramble_theme.h"
#include "screens/scr_layout.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "ui_gfx";
bramble_layout_t *s_layout = NULL;  /* NOT static — screens need access */

int ui_graphics_init(void) {
    ESP_LOGI(TAG, "Initializing LVGL graphical UI");
    lv_init();
    
    lv_display_t *disp = lv_port_display_init();
    if (!disp) {
        ESP_LOGE(TAG, "Display port init failed");
        return -1;
    }
    
    bramble_theme_init(disp);
    lv_port_touch_init();
    lv_port_trackball_init();
    lv_port_keyboard_init();
    
    s_layout = layout_create();
    
    ESP_LOGI(TAG, "LVGL initialized with layout");
    return 0;
}

uint32_t ui_graphics_tick(void) {
    return lv_timer_handler();
}

void ui_graphics_notify(uint32_t event_mask) {
    (void)event_mask;
}
