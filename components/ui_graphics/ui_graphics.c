#include "ui_graphics.h"
#include "lv_port_display.h"
#include "lv_port_touch.h"
#include "lv_port_trackball.h"
#include "lv_port_keyboard.h"
#include "theme/bramble_theme.h"
#include "screens/scr_layout.h"
#include "screens/scr_splash.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "ui_gfx";
bramble_layout_t *s_layout = NULL;  /* NOT static — screens need access */

static lv_display_t *s_display = NULL;

/* Timer callbacks for periodic refresh */
static void status_refresh_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_layout) {
        layout_update_status(s_layout);
    }
}

static void tab_refresh_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (!s_layout) return;
    
    /* Only refresh Stats and Nodes tabs (not Chat or Settings) */
    if (s_layout->active_tab == TAB_STATS || s_layout->active_tab == TAB_NODES) {
        layout_set_tab(s_layout, s_layout->active_tab);
    }
}

/* Timer callback to transition from splash to main UI */
static void splash_timer_cb(lv_timer_t *timer) {
    ESP_LOGI(TAG, "Splash timeout — transitioning to main UI");
    
    /* Delete splash screen */
    lv_obj_t *splash = lv_screen_active();
    if (splash) {
        lv_obj_delete(splash);
    }
    
    /* Initialize theme and main UI */
    bramble_theme_init(s_display);
    lv_port_touch_init();
    lv_port_trackball_init();
    lv_port_keyboard_init();
    
    s_layout = layout_create();
    
    /* Create periodic refresh timers */
    lv_timer_create(status_refresh_timer_cb, 2000, NULL);  /* Status bar: 2s */
    lv_timer_create(tab_refresh_timer_cb, 5000, NULL);     /* Tab content: 5s */
    
    /* One-shot timer — delete itself */
    lv_timer_delete(timer);
}

int ui_graphics_init(void) {
    ESP_LOGI(TAG, "Initializing LVGL graphical UI");
    lv_init();
    
    lv_display_t *disp = lv_port_display_init();
    if (!disp) {
        ESP_LOGE(TAG, "Display port init failed");
        return -1;
    }
    
    s_display = disp;
    
    /* Show splash screen */
    scr_splash_create(disp);
    
    /* Create one-shot timer to transition to main UI after 2 seconds */
    lv_timer_create(splash_timer_cb, 2000, NULL);
    
    ESP_LOGI(TAG, "LVGL initialized with splash screen");
    return 0;
}

uint32_t ui_graphics_tick(void) {
    return lv_timer_handler();
}

void ui_graphics_notify(uint32_t event_mask) {
    (void)event_mask;
}

void ui_graphics_tick_1ms(void) {
    lv_tick_inc(1);
}
