#include "ui_graphics.h"
#include "lv_port_display.h"
#include "lv_port_touch.h"
#include "lv_port_trackball.h"
#include "lv_port_keyboard.h"
#include "theme/bramble_theme.h"
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
    
    bramble_theme_init(disp);
    
    lv_port_touch_init();
    lv_port_trackball_init();
    lv_port_keyboard_init();
    
    /* Temporary test — will be replaced by layout in Task 7 */
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "BRAMBLE");
    lv_obj_set_style_text_color(label, lv_color_hex(0x0F9B8E), 0);
    lv_obj_center(label);
    
    ESP_LOGI(TAG, "LVGL initialized with theme and all inputs");
    return 0;
}

uint32_t ui_graphics_tick(void) {
    return lv_timer_handler();
}

void ui_graphics_notify(uint32_t event_mask) {
    (void)event_mask;
}
