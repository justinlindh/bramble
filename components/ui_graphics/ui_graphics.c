#include "ui_graphics.h"
#include "lv_port_display.h"
#include "lv_port_touch.h"
#include "lv_port_trackball.h"
#include "lv_port_keyboard.h"
#include "sleep_manager.h"
#include "theme/bramble_theme.h"
#include "screens/scr_layout.h"
#include "screens/scr_splash.h"
#include "chat_unread.h"
#include "screens/scr_chat_messages.h"
#include "msg_store.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "ui_gfx";
bramble_layout_t *s_layout = NULL;  /* NOT static — screens need access */

static lv_display_t *s_display = NULL;
static uint32_t s_pending_events = 0;
static uint32_t s_pending_msg_received = 0;
static int s_unread_count = 0;

static void process_new_message_unread(uint32_t arrivals) {
    int count = msg_store_count();
    if (count <= 0 || arrivals == 0) {
        return;
    }

    /* Process newest N messages, clamped to store depth for ring-buffer safety. */
    int to_process = (arrivals > (uint32_t)count) ? count : (int)arrivals;
    int start = count - to_process;

    for (int i = start; i < count; i++) {
        const stored_msg_t *msg = msg_store_get(i);
        chat_unread_mark_for_message(msg);
    }
}

/* Timer callbacks for periodic refresh */
static void status_refresh_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_layout) {
        layout_update_status(s_layout);
    }
    
    /* Check for pending notifications from other tasks */
    uint32_t events = __atomic_exchange_n(&s_pending_events, 0, __ATOMIC_ACQ_REL);
    uint32_t msg_received = __atomic_exchange_n(&s_pending_msg_received, 0, __ATOMIC_ACQ_REL);

    if ((events & UI_EVT_MSG_RECEIVED) && msg_received > 0) {
        process_new_message_unread(msg_received);

        if (s_layout) {
            if (s_layout->active_tab == TAB_CHAT) {
                /* Chat is active — refresh list/bubbles unless user is in a DM view */
                if (s_layout->in_dm_view) {
                    scr_chat_messages_on_recv();
                } else {
                    layout_set_tab(s_layout, TAB_CHAT);
                }
                s_unread_count = 0;
                layout_set_unread(s_layout, 0);
            } else {
                /* Chat is not active - increment global chat tab unread */
                s_unread_count++;
                layout_set_unread(s_layout, s_unread_count);
            }
        }
    }
}

static void tab_refresh_timer_cb(lv_timer_t *timer) {
    (void)timer;
    /* Intentionally empty — data screens show a snapshot; user switches
     * tabs to refresh.  The old implementation called layout_set_tab()
     * every 5 s, which destroyed scroll position and drill-down views. */
}

/* Timer callback to transition from splash to main UI */
static void splash_timer_cb(lv_timer_t *timer) {
    ESP_LOGI(TAG, "Splash timeout — transitioning to main UI");
    
    /* Initialize theme — applies to the active screen (still splash).
     * layout_create() will build the main UI on this screen. */
    bramble_theme_init(s_display);
    
    /* Force LVGL to complete all pending layout/render work before cleaning
     * the splash screen. The flex container (LV_SIZE_CONTENT) leaves pending
     * layout tasks in LVGL's queue. If we call lv_obj_clean() while these are
     * outstanding, the timer linked list can get corrupted — which silently
     * kills the indev read timer and breaks keyboard/trackball input.
     * lv_refr_now() flushes all pending layouts before we delete anything. */
    lv_refr_now(s_display);

    /* Clean the splash content before building main UI */
    lv_obj_clean(lv_screen_active());
    lv_port_touch_init();
    lv_port_trackball_init();
    lv_port_keyboard_init();
    
    s_layout = layout_create();
    
    /* Create periodic refresh timers */
    lv_timer_create(status_refresh_timer_cb, 2000, NULL);  /* Status bar: 2s */
    lv_timer_create(tab_refresh_timer_cb, 5000, NULL);     /* Tab content: 5s */
    
    /* Initialize sleep manager for automatic display power saving */
    sleep_manager_init();
    
    /* One-shot timer — delete itself */
    lv_timer_delete(timer);
}

int ui_graphics_init(void) {
    ESP_LOGI(TAG, "Initializing LVGL graphical UI");
    lv_init();

    s_unread_count = 0;
    __atomic_store_n(&s_pending_events, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pending_msg_received, 0, __ATOMIC_RELEASE);
    chat_unread_reset();
    
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
    __atomic_fetch_or(&s_pending_events, event_mask, __ATOMIC_RELEASE);

    if (event_mask & UI_EVT_MSG_RECEIVED) {
        __atomic_add_fetch(&s_pending_msg_received, 1, __ATOMIC_RELEASE);
    }
}

void ui_graphics_clear_unread(void) {
    s_unread_count = 0;
}

void ui_graphics_tick_1ms(void) {
    lv_tick_inc(1);
}
