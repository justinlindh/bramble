#include "scr_layout.h"
#include "scr_chat_list.h"
#include "scr_nodes.h"
#include "scr_stats.h"
#include "scr_settings.h"
#include "theme/bramble_theme.h"
#include "battery.h"
#include "routing.h"
#include "airtime_budget.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "layout";
static bramble_layout_t s_layout;

/* Duplicate mesh state struct (mesh_task.h is in main, not component) */
typedef struct {
    neighbor_table_t neighbors;
    uint32_t         beacon_tx_count;
    uint32_t         beacon_rx_count;
    uint32_t         packets_tx;
    uint32_t         packets_rx;
    bool             radio_ok;
    int16_t          last_rx_rssi;
    int8_t           last_rx_snr;
    airtime_budget_t airtime;
} ui_mesh_state_t;

extern void mesh_get_state(ui_mesh_state_t *out);

static const char *tab_labels[TAB_COUNT] = {
    LV_SYMBOL_ENVELOPE " Chat",
    LV_SYMBOL_WIFI " Nodes",
    LV_SYMBOL_BARS " Stats",
    LV_SYMBOL_SETTINGS " Set"
};

static void tab_click_cb(lv_event_t *e) {
    bramble_tab_t tab = (bramble_tab_t)(intptr_t)lv_event_get_user_data(e);
    layout_set_tab(&s_layout, tab);
}

bramble_layout_t *layout_create(void) {
    lv_obj_t *scr = lv_screen_active();
    s_layout.screen = scr;
    lv_obj_set_style_bg_color(scr, BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* === Status Bar (top 20px) === */
    s_layout.status_bar = lv_obj_create(scr);
    lv_obj_set_size(s_layout.status_bar, 320, BR_STATUS_BAR_H);
    lv_obj_set_pos(s_layout.status_bar, 0, 0);
    lv_obj_set_style_bg_color(s_layout.status_bar, lv_color_hex(0x111122), 0);
    lv_obj_set_style_bg_opa(s_layout.status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_layout.status_bar, 0, 0);
    lv_obj_set_style_border_width(s_layout.status_bar, 0, 0);
    lv_obj_set_style_pad_all(s_layout.status_bar, 2, 0);
    lv_obj_clear_flag(s_layout.status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_layout.status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_layout.status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_layout.lbl_battery = lv_label_create(s_layout.status_bar);
    lv_label_set_text(s_layout.lbl_battery, LV_SYMBOL_BATTERY_FULL " 100%");
    lv_obj_set_style_text_font(s_layout.lbl_battery, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_layout.lbl_battery, BR_COLOR_TEXT, 0);

    s_layout.lbl_signal = lv_label_create(s_layout.status_bar);
    lv_label_set_text(s_layout.lbl_signal, LV_SYMBOL_WIFI " 0");
    lv_obj_set_style_text_font(s_layout.lbl_signal, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_layout.lbl_signal, BR_COLOR_TEXT, 0);

    s_layout.lbl_gps = lv_label_create(s_layout.status_bar);
    lv_label_set_text(s_layout.lbl_gps, "GPS");
    lv_obj_set_style_text_font(s_layout.lbl_gps, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_layout.lbl_gps, BR_COLOR_TEXT_SEC, 0);

    s_layout.lbl_time = lv_label_create(s_layout.status_bar);
    lv_label_set_text(s_layout.lbl_time, "--:--");
    lv_obj_set_style_text_font(s_layout.lbl_time, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_layout.lbl_time, BR_COLOR_TEXT, 0);

    s_layout.lbl_node_name = lv_label_create(s_layout.status_bar);
    lv_label_set_text(s_layout.lbl_node_name, "BRAMBLE");
    lv_obj_set_style_text_font(s_layout.lbl_node_name, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_layout.lbl_node_name, BR_COLOR_PRIMARY, 0);

    /* === Content Area (middle 180px) === */
    s_layout.content_area = lv_obj_create(scr);
    lv_obj_set_size(s_layout.content_area, 320, BR_CONTENT_H);
    lv_obj_set_pos(s_layout.content_area, 0, BR_STATUS_BAR_H);
    lv_obj_set_style_bg_color(s_layout.content_area, BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_layout.content_area, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_layout.content_area, 0, 0);
    lv_obj_set_style_border_width(s_layout.content_area, 0, 0);
    lv_obj_set_style_pad_all(s_layout.content_area, 0, 0);

    /* === Tab Bar (bottom 40px) === */
    s_layout.tab_bar = lv_obj_create(scr);
    lv_obj_set_size(s_layout.tab_bar, 320, BR_TAB_BAR_H);
    lv_obj_set_pos(s_layout.tab_bar, 0, 240 - BR_TAB_BAR_H);
    lv_obj_set_style_bg_color(s_layout.tab_bar, lv_color_hex(0x111122), 0);
    lv_obj_set_style_bg_opa(s_layout.tab_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_layout.tab_bar, 0, 0);
    lv_obj_set_style_border_width(s_layout.tab_bar, 0, 0);
    lv_obj_set_style_pad_all(s_layout.tab_bar, 0, 0);
    lv_obj_clear_flag(s_layout.tab_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_layout.tab_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_layout.tab_bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *btn = lv_btn_create(s_layout.tab_bar);
        lv_obj_set_size(btn, 75, 36);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, tab_labels[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
        
        lv_obj_add_event_cb(btn, tab_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        
        s_layout.tab_btns[i] = btn;
    }

    s_layout.active_tab = TAB_CHAT;
    layout_set_tab(&s_layout, TAB_CHAT);

    ESP_LOGI(TAG, "Layout created");
    return &s_layout;
}

void layout_set_tab(bramble_layout_t *layout, bramble_tab_t tab) {
    if (tab >= TAB_COUNT) return;
    
    for (int i = 0; i < TAB_COUNT; i++) {
        if (i == (int)tab) {
            lv_obj_set_style_bg_color(layout->tab_btns[i], BR_COLOR_PRIMARY, 0);
            lv_obj_set_style_bg_opa(layout->tab_btns[i], LV_OPA_30, 0);
        } else {
            lv_obj_set_style_bg_opa(layout->tab_btns[i], LV_OPA_TRANSP, 0);
        }
    }
    
    lv_obj_clean(layout->content_area);
    layout->active_tab = tab;
    
    /* Clear unread badge when switching to Chat tab */
    if (tab == TAB_CHAT) {
        extern void ui_graphics_clear_unread(void);
        ui_graphics_clear_unread();
        layout_set_unread(layout, 0);
    }
    
    switch (tab) {
    case TAB_CHAT:
        scr_chat_list_create(layout);
        break;
    case TAB_NODES:
        scr_nodes_create(layout);
        break;
    case TAB_STATS:
        scr_stats_create(layout);
        break;
    case TAB_SETTINGS:
        scr_settings_create(layout);
        break;
    case TAB_COUNT:
        break;
    }
}

void layout_update_status(bramble_layout_t *layout) {
    /* Battery */
    int pct = battery_read_pct();
    char buf[32];
    const char *batt_sym = pct > 75 ? LV_SYMBOL_BATTERY_FULL :
                           pct > 50 ? LV_SYMBOL_BATTERY_3 :
                           pct > 25 ? LV_SYMBOL_BATTERY_2 :
                                      LV_SYMBOL_BATTERY_1;
    snprintf(buf, sizeof(buf), "%s %d%%", batt_sym, pct);
    lv_label_set_text(layout->lbl_battery, buf);
    
    if (pct <= 15) {
        lv_obj_set_style_text_color(layout->lbl_battery, BR_COLOR_DANGER, 0);
    } else if (pct <= 30) {
        lv_obj_set_style_text_color(layout->lbl_battery, BR_COLOR_ACCENT, 0);
    } else {
        lv_obj_set_style_text_color(layout->lbl_battery, BR_COLOR_TEXT, 0);
    }

    /* Neighbor count (signal strength indicator) */
    static ui_mesh_state_t state;
    mesh_get_state(&state);
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %d", state.neighbors.count);
    lv_label_set_text(layout->lbl_signal, buf);

    /* Node name - keeping static "BRAMBLE" for now */
    /* identity component doesn't provide a get_name() function yet */
}

void layout_set_unread(bramble_layout_t *layout, int count) {
    if (count > 0) {
        /* Create or update badge */
        if (layout->chat_badge == NULL) {
            /* Create badge container */
            layout->chat_badge = lv_obj_create(layout->tab_btns[TAB_CHAT]);
            lv_obj_set_size(layout->chat_badge, 20, 20);
            lv_obj_set_style_bg_color(layout->chat_badge, lv_color_hex(0xFF0000), 0);
            lv_obj_set_style_bg_opa(layout->chat_badge, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(layout->chat_badge, 10, 0);  /* Circle */
            lv_obj_set_style_border_width(layout->chat_badge, 0, 0);
            lv_obj_set_style_pad_all(layout->chat_badge, 0, 0);
            lv_obj_align(layout->chat_badge, LV_ALIGN_TOP_RIGHT, -2, 2);
            lv_obj_clear_flag(layout->chat_badge, LV_OBJ_FLAG_SCROLLABLE);
            
            /* Add label with count */
            lv_obj_t *lbl = lv_label_create(layout->chat_badge);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
            lv_obj_center(lbl);
        }
        
        /* Update count text */
        lv_obj_t *lbl = lv_obj_get_child(layout->chat_badge, 0);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", count > 99 ? 99 : count);
        lv_label_set_text(lbl, buf);
    } else {
        /* Remove badge if count is 0 */
        if (layout->chat_badge != NULL) {
            lv_obj_delete(layout->chat_badge);
            layout->chat_badge = NULL;
        }
    }
}

lv_obj_t *layout_get_content(bramble_layout_t *layout) {
    return layout->content_area;
}
