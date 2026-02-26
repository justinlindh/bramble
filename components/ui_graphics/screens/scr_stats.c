#include "scr_stats.h"
#include "theme/bramble_theme.h"
#include "routing.h"
#include "airtime_budget.h"
#include "traffic_debug.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

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
extern void mesh_get_routes(routing_table_t *out);
extern traffic_debug_t *mesh_get_traffic_debug(void);

typedef enum {
    DELTA_GOOD_WHEN_UP,
    DELTA_GOOD_WHEN_DOWN,
} delta_polarity_t;

typedef struct {
    int32_t tx;
    int32_t rx;
    int32_t dropped;
    int32_t neighbors;
    int32_t routes;
    int32_t air_used_ms;
} stats_delta_t;

typedef struct {
    uint32_t tx;
    uint32_t rx;
    uint32_t dropped;
    uint32_t neighbors;
    uint32_t routes;
    uint32_t air_used_ms;
} stats_snapshot_t;

static stats_snapshot_t s_prev_snapshot;
static bool s_has_prev_snapshot = false;

/* Format milliseconds as a short human-readable string */
static void fmt_ms_short(char *buf, size_t sz, uint32_t ms) {
    if (ms >= 60000) {
        uint32_t m = ms / 60000;
        uint32_t s = (ms % 60000) / 1000;
        if (s > 0) {
            snprintf(buf, sz, "%um%us", (unsigned)m, (unsigned)s);
        } else {
            snprintf(buf, sz, "%um", (unsigned)m);
        }
    } else if (ms >= 1000) {
        snprintf(buf, sz, "%us", (unsigned)(ms / 1000));
    } else {
        snprintf(buf, sz, "%ums", (unsigned)ms);
    }
}

/*
 * Create one airtime tier row: label (with tier name and remaining/max),
 * plus a filled progress bar beneath it.
 */
static void create_airtime_row(lv_obj_t *parent,
                                const char *name,
                                uint32_t remaining_ms, uint32_t max_ms,
                                lv_color_t bar_color) {
    /* Column container for the header + bar */
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_size(col, 296, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    /* Compute percentage */
    int pct = (max_ms > 0) ? (int)((remaining_ms * 100u) / max_ms) : 0;
    if (pct > 100) pct = 100;

    /* Header row: tier name (left) + "rem/max pct%" (right) */
    lv_obj_t *hdr = lv_obj_create(col);
    lv_obj_set_size(hdr, 296, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name_lbl = lv_label_create(hdr);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(name_lbl, bar_color, 0);

    char rem_str[10], max_str[10], val_buf[40];
    fmt_ms_short(rem_str, sizeof(rem_str), remaining_ms);
    fmt_ms_short(max_str, sizeof(max_str), max_ms);
    snprintf(val_buf, sizeof(val_buf), "%s/%s %d%%", rem_str, max_str, pct);

    lv_obj_t *val_lbl = lv_label_create(hdr);
    lv_label_set_text(val_lbl, val_buf);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(val_lbl, BR_COLOR_TEXT_SEC, 0);

    /* Progress bar */
    lv_obj_t *bar = lv_bar_create(col);
    lv_obj_set_size(bar, 296, 6);
    lv_obj_set_style_bg_color(bar, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_bg_color(bar, bar_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
}

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
    const char *ip = wifi_manager_get_ip();
    
    char sys_buf[160];
    if (ip && ip[0] != '\0') {
        snprintf(sys_buf, sizeof(sys_buf),
                 "Free heap:  %u KB\n"
                 "PSRAM free: %.1f MB\n"
                 "Uptime:     %luh %lum\n"
                 "IP address: %s",
                 (unsigned)(free_heap / 1024),
                 free_psram / (1024.0 * 1024.0),
                 (unsigned long)up_h,
                 (unsigned long)up_m,
                 ip);
    } else {
        snprintf(sys_buf, sizeof(sys_buf),
                 "Free heap:  %u KB\n"
                 "PSRAM free: %.1f MB\n"
                 "Uptime:     %luh %lum\n"
                 "IP address: (WiFi off)",
                 (unsigned)(free_heap / 1024),
                 free_psram / (1024.0 * 1024.0),
                 (unsigned long)up_h,
                 (unsigned long)up_m);
    }
    
    lv_obj_t *sys_lbl = lv_label_create(cont);
    lv_label_set_text(sys_lbl, sys_buf);
    lv_obj_set_style_text_font(sys_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sys_lbl, BR_COLOR_TEXT, 0);

    /* ---- Airtime Budget section ---- */
    lv_obj_t *sep2 = lv_obj_create(cont);
    lv_obj_set_size(sep2, 296, 1);
    lv_obj_set_style_bg_color(sep2, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep2, 0, 0);

    lv_obj_t *air_title = lv_label_create(cont);
    lv_label_set_text(air_title, "Airtime Budget");
    lv_obj_set_style_text_font(air_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(air_title, BR_COLOR_TEXT_SEC, 0);

    /* Critical (tier 1) first — highest priority */
    create_airtime_row(cont, "Critical",
                       state.airtime.tokens_ms[1],
                       state.airtime.max_ms[1],
                       BR_COLOR_CRITICAL);

    /* Normal (tier 0) */
    create_airtime_row(cont, "Normal",
                       state.airtime.tokens_ms[0],
                       state.airtime.max_ms[0],
                       BR_COLOR_PRIMARY);

    /* Broadcast (tier 2) */
    create_airtime_row(cont, "Broadcast",
                       state.airtime.tokens_ms[2],
                       state.airtime.max_ms[2],
                       BR_COLOR_WARNING);
}
