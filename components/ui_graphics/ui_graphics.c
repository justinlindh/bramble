#include "ui_graphics.h"
#include "lv_port_display.h"
#include "lv_port_touch.h"
#include "lv_port_trackball.h"
#include "lv_port_keyboard.h"
#include "ui_zone.h"
#include "sleep_manager.h"
#include "theme/bramble_theme.h"
#include "screens/scr_layout.h"
#include "screens/scr_splash.h"
#include "chat_unread.h"
#include "chat_target.h"
#include "screens/scr_chat_messages.h"
#include "msg_store.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>

static const char* TAG = "ui_gfx";
bramble_layout_t* s_layout = NULL; /* NOT static — screens need access */

static lv_display_t* s_display = NULL;
static uint32_t s_pending_events = 0;
static uint32_t s_pending_msg_received = 0;
static int s_unread_count = 0;

/* Bench debug screenshot state. s_shot_buf is allocated lazily on first
 * request (reused after) so boards/builds that never call the RPC never pay
 * for it. s_shot_requester_mutex serializes concurrent RPC callers (only
 * one capture can be in flight); s_shot_done is given by the UI task when a
 * requested capture completes (success or failure). */
#define UI_SCREENSHOT_BUF_SIZE (UI_SCREENSHOT_LEN + 256) /* lv_snapshot alignment slack */
static uint8_t* s_shot_buf = NULL;
static const uint8_t* s_shot_data = NULL; /* pixel data start within s_shot_buf */
static size_t s_shot_len = 0;
static volatile bool s_shot_requested = false;
static volatile bool s_shot_ok = false;
static SemaphoreHandle_t s_shot_done = NULL;
static SemaphoreHandle_t s_shot_requester_mutex = NULL;

/* Mark the new arrivals unread, EXCEPT any belonging to the chat thread that is
 * currently on screen: the user is looking at those, so counting them unread
 * would badge a conversation they are already reading. Returns true if at least
 * one arrival belongs to the open thread, i.e. the open view needs a repaint.
 * open_target is NULL when no chat thread is open. */
static bool process_new_message_unread(uint32_t arrivals, const chat_target_t* open_target) {
    int count = msg_store_count();
    if (count <= 0 || arrivals == 0) {
        return false;
    }

    /* Process newest N messages, clamped to store depth for ring-buffer safety. */
    int to_process = (arrivals > (uint32_t)count) ? count : (int)arrivals;
    int start = count - to_process;

    bool for_open_chat = false;
    for (int i = start; i < count; i++) {
        stored_msg_t msg;
        if (!msg_store_get_copy(i, &msg))
            continue;
        if (open_target &&
            chat_target_matches_message(*open_target, &msg, (int)msg.channel_index)) {
            for_open_chat = true;
            continue;
        }
        chat_unread_mark_for_message(&msg);
    }
    return for_open_chat;
}

/* Timer callbacks for periodic refresh */
static void status_refresh_timer_cb(lv_timer_t* timer) {
    (void)timer;
    if (s_layout) {
        layout_update_status(s_layout);
    }

    /* Check for pending notifications from other tasks */
    uint32_t events = __atomic_exchange_n(&s_pending_events, 0, __ATOMIC_ACQ_REL);
    uint32_t msg_received = __atomic_exchange_n(&s_pending_msg_received, 0, __ATOMIC_ACQ_REL);

    if ((events & UI_EVT_MSG_RECEIVED) && msg_received > 0) {
        /* A message is user-relevant activity: wake the display so the
         * unread badge is actually visible. */
        sleep_manager_activity();

        /* Ask the chat screen what is open, not the nav tab: a DM opened from
         * node detail leaves active_tab == TAB_NODES, and gating the repaint on
         * TAB_CHAT is why an open thread only showed an arrival after you left
         * and re-entered it. */
        chat_target_t open_target;
        bool chat_open = scr_chat_messages_open_target(&open_target);
        bool for_open_chat =
            process_new_message_unread(msg_received, chat_open ? &open_target : NULL);

        if (s_layout) {
            if (chat_open) {
                /* A thread is on screen. Repaint it in place when the arrival
                 * is one of its own; otherwise it belongs to another
                 * conversation and bumps the tab badge like any background
                 * message. */
                if (for_open_chat) {
                    scr_chat_messages_on_recv();
                } else {
                    s_unread_count++;
                    layout_set_unread(s_layout, s_unread_count);
                }
            } else if (s_layout->active_tab == TAB_CHAT) {
                /* The chat LIST is on screen: rebuild it so the per-row unread
                 * badges update, and clear the tab badge, since every
                 * conversation with an arrival is now visible in the list. */
                layout_set_tab(s_layout, TAB_CHAT);
                s_unread_count = 0;
                layout_set_unread(s_layout, 0);
            } else {
                s_unread_count++;
                layout_set_unread(s_layout, s_unread_count);
            }
        }
    }

    if (events & UI_EVT_MSG_STATUS) {
        /* A delivery status changed under an open thread: repaint it so the
         * badge advances from pending to the delivered double check. Deliberately
         * no unread bookkeeping and no display wake: nothing arrived, one of our
         * own bubbles just got confirmed, and waking the screen for that would
         * light the device up every time an ACK trickles in. Repaint only when a
         * thread is actually on screen; there is nothing to update otherwise. */
        chat_target_t status_target;
        if (scr_chat_messages_open_target(&status_target)) {
            scr_chat_messages_on_recv();
        }
    }
}

/* Drive the sleep manager's blocking display power-down on the UI task. The
 * inactivity esp_timer only raises a flag; the actual SPI work happens here so
 * it never stalls the esp_timer service task (and the 1 ms lv_tick). */
static void sleep_process_timer_cb(lv_timer_t* timer) {
    (void)timer;
    sleep_manager_process();
}

static void tab_refresh_timer_cb(lv_timer_t* timer) {
    (void)timer;
    /* Intentionally empty — data screens show a snapshot; user switches
     * tabs to refresh.  The old implementation called layout_set_tab()
     * every 5 s, which destroyed scroll position and drill-down views. */
}

/* Timer callback to transition from splash to main UI */
static void splash_timer_cb(lv_timer_t* timer) {
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
    /* Create the content/chrome focus groups (content becomes the default
     * group) before any indev is bound to a group. */
    ui_zone_init();
    lv_port_touch_init();
    lv_port_trackball_init();
    lv_port_keyboard_init();

    s_layout = layout_create();

    /* Create periodic refresh timers */
    lv_timer_create(status_refresh_timer_cb, 2000, NULL); /* Status bar: 2s */
    lv_timer_create(tab_refresh_timer_cb, 5000, NULL);    /* Tab content: 5s */
    lv_timer_create(sleep_process_timer_cb, 500, NULL);   /* Sleep drive: 0.5s */

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

    lv_display_t* disp = lv_port_display_init();
    if (!disp) {
        ESP_LOGE(TAG, "Display port init failed");
        return -1;
    }

    s_display = disp;

    s_shot_requested = false;
    s_shot_ok = false;
    s_shot_done = xSemaphoreCreateBinary();
    s_shot_requester_mutex = xSemaphoreCreateMutex();
    if (!s_shot_done || !s_shot_requester_mutex) {
        /* Non-fatal: the graphical UI still works, only bramble.screenshot
         * will time out on every call. */
        ESP_LOGE(TAG, "Failed to create screenshot sync primitives");
    }

    /* Show splash screen */
    scr_splash_create(disp);

    /* Create one-shot timer to transition to main UI after 2 seconds */
    lv_timer_create(splash_timer_cb, 2000, NULL);

    ESP_LOGI(TAG, "LVGL initialized with splash screen");
    return 0;
}

/* Services a pending screenshot request. MUST only run on the LVGL-owning
 * task (called from ui_graphics_tick below) since lv_snapshot_take_to_buf
 * walks the live object tree exactly like a render pass would. */
static void service_screenshot_request(void) {
    if (!__atomic_exchange_n(&s_shot_requested, false, __ATOMIC_ACQ_REL))
        return;

    if (!s_shot_buf) {
        /* Lazy allocate once, reuse after. PSRAM first, internal RAM
         * fallback, same pattern as mesh_task.c's s_dm_table. */
        s_shot_buf = heap_caps_calloc(1, UI_SCREENSHOT_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_shot_buf) {
            ESP_LOGW(TAG, "No PSRAM for screenshot buffer, using internal RAM (%u bytes)",
                     (unsigned)UI_SCREENSHOT_BUF_SIZE);
            s_shot_buf = calloc(1, UI_SCREENSHOT_BUF_SIZE);
        }
        if (!s_shot_buf) {
            ESP_LOGE(TAG, "Failed to allocate screenshot buffer (%u bytes)",
                     (unsigned)UI_SCREENSHOT_BUF_SIZE);
            s_shot_ok = false;
            xSemaphoreGive(s_shot_done);
            return;
        }
    }

    lv_image_dsc_t dsc;
    lv_result_t res = lv_snapshot_take_to_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &dsc,
                                              s_shot_buf, UI_SCREENSHOT_BUF_SIZE);
    if (res != LV_RESULT_OK || !dsc.data) {
        ESP_LOGW(TAG, "lv_snapshot_take_to_buf failed (res=%d)", (int)res);
        s_shot_ok = false;
        xSemaphoreGive(s_shot_done);
        return;
    }

    size_t len = (size_t)dsc.header.h * (size_t)dsc.header.stride;
    if (dsc.header.w != UI_SCREENSHOT_WIDTH || dsc.header.h != UI_SCREENSHOT_HEIGHT ||
        len > UI_SCREENSHOT_LEN) {
        /* Defensive: current LVGL config always yields exactly 320x240
         * RGB565 with a tight (unpadded) stride, but a future LVGL/config
         * change (e.g. LV_DRAW_BUF_STRIDE_ALIGN) could silently break the
         * fixed-size assumption the RPC contract makes. Fail loud rather
         * than ship a mis-sized or truncated frame. */
        ESP_LOGE(TAG, "Unexpected snapshot shape %ux%u stride=%u (want %dx%d)",
                 (unsigned)dsc.header.w, (unsigned)dsc.header.h, (unsigned)dsc.header.stride,
                 UI_SCREENSHOT_WIDTH, UI_SCREENSHOT_HEIGHT);
        s_shot_ok = false;
        xSemaphoreGive(s_shot_done);
        return;
    }

    s_shot_data = dsc.data;
    s_shot_len = len;
    s_shot_ok = true;
    xSemaphoreGive(s_shot_done);
}

uint32_t ui_graphics_tick(void) {
    uint32_t delay = lv_timer_handler();
    service_screenshot_request();
    return delay;
}

bool ui_graphics_request_screenshot(uint32_t timeout_ms) {
    if (!s_shot_done || !s_shot_requester_mutex)
        return false;
    if (xSemaphoreTake(s_shot_requester_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return false;

    /* Drain any stale "done" signal from a prior timed-out request before
     * arming a new one. */
    xSemaphoreTake(s_shot_done, 0);
    __atomic_store_n(&s_shot_requested, true, __ATOMIC_RELEASE);

    bool ok = false;
    if (xSemaphoreTake(s_shot_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        ok = s_shot_ok;
    } else {
        ESP_LOGW(TAG, "Screenshot request timed out");
    }

    xSemaphoreGive(s_shot_requester_mutex);
    return ok;
}

const uint8_t* ui_graphics_get_screenshot(size_t* out_len) {
    if (!s_shot_data || !s_shot_ok)
        return NULL;
    if (out_len)
        *out_len = s_shot_len;
    return s_shot_data;
}

void ui_graphics_notify(uint32_t event_mask) {
    __atomic_fetch_or(&s_pending_events, event_mask, __ATOMIC_RELEASE);

    if (event_mask & UI_EVT_MSG_RECEIVED) {
        __atomic_add_fetch(&s_pending_msg_received, 1, __ATOMIC_RELEASE);
    }
}

void ui_graphics_clear_unread(void) { s_unread_count = 0; }

void ui_graphics_tick_1ms(void) { lv_tick_inc(1); }
