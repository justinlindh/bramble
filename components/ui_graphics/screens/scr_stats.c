#include "scr_stats.h"
#include "theme/bramble_theme.h"
#include "routing.h"
#include "airtime_budget.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>

static const char *TAG = "scr_stats";

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

static void create_counter_card(lv_obj_t *parent, int value, const char *unit) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 96, 52);
    lv_obj_set_style_bg_color(card, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, BR_RADIUS, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    
    char val_buf[16];
    snprintf(val_buf, sizeof(val_buf), "%d", value);
    
    lv_obj_t *val_lbl = lv_label_create(card);
    lv_label_set_text(val_lbl, val_buf);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(val_lbl, BR_COLOR_PRIMARY, 0);
    
    lv_obj_t *name_lbl = lv_label_create(card);
    lv_label_set_text(name_lbl, unit);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(name_lbl, BR_COLOR_TEXT_SEC, 0);
}

void scr_stats_create(bramble_layout_t *layout) {
    lv_obj_t *cont = layout_get_content(layout);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, BR_PADDING, 0);
    lv_obj_set_style_pad_row(cont, 6, 0);
    
    /* Get mesh state */
    static ui_mesh_state_t state;
    mesh_get_state(&state);
    
    /* Title */
    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "Stats");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    
    /* Counter row */
    lv_obj_t *row = lv_obj_create(cont);
    lv_obj_set_size(row, 304, 56);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    
    create_counter_card(row, (int)state.packets_tx, "TX pkts");
    create_counter_card(row, (int)state.packets_rx, "RX pkts");
    create_counter_card(row, state.neighbors.count, "peers");
    
    /* Radio status */
    lv_obj_t *radio_lbl = lv_label_create(cont);
    char radio_buf[48];
    snprintf(radio_buf, sizeof(radio_buf), "Radio: %s  Last RSSI: %ddBm",
             state.radio_ok ? "OK" : "ERROR", state.last_rx_rssi);
    lv_label_set_text(radio_lbl, radio_buf);
    lv_obj_set_style_text_font(radio_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(radio_lbl, state.radio_ok ? BR_COLOR_TEXT_SEC : BR_COLOR_DANGER, 0);
    
    /* Separator */
    lv_obj_t *sep = lv_obj_create(cont);
    lv_obj_set_size(sep, 296, 1);
    lv_obj_set_style_bg_color(sep, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    
    /* System info */
    lv_obj_t *sys_title = lv_label_create(cont);
    lv_label_set_text(sys_title, "System");
    lv_obj_set_style_text_font(sys_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sys_title, BR_COLOR_TEXT_SEC, 0);
    
    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t uptime_us = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t up_h = uptime_us / 3600;
    uint32_t up_m = (uptime_us % 3600) / 60;
    
    char sys_buf[128];
    snprintf(sys_buf, sizeof(sys_buf),
             "Free heap:  %u KB\n"
             "PSRAM free: %.1f MB\n"
             "Uptime:     %luh %lum",
             (unsigned)(free_heap / 1024),
             free_psram / (1024.0 * 1024.0),
             (unsigned long)up_h,
             (unsigned long)up_m);
    
    lv_obj_t *sys_lbl = lv_label_create(cont);
    lv_label_set_text(sys_lbl, sys_buf);
    lv_obj_set_style_text_font(sys_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sys_lbl, BR_COLOR_TEXT, 0);
}
