#include "scr_stats.h"
#include "ui_shared_state.h"
#include "ui_zone.h"
#include "ui_toast.h"
#include "scr_traffic.h"
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

static const char* TAG = "scr_stats";

extern void mesh_get_routes(routing_table_t* out);
extern traffic_debug_t* mesh_get_traffic_debug(void);

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
static void fmt_ms_short(char* buf, size_t sz, uint32_t ms) {
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
static void create_airtime_row(lv_obj_t* parent, const char* name, uint32_t remaining_ms,
                               uint32_t max_ms, lv_color_t bar_color) {
    lv_obj_t* col = lv_obj_create(parent);
    lv_obj_set_size(col, 296, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    int pct = (max_ms > 0) ? (int)((remaining_ms * 100u) / max_ms) : 0;
    if (pct > 100)
        pct = 100;

    lv_obj_t* hdr = lv_obj_create(col);
    lv_obj_set_size(hdr, 296, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* name_lbl = lv_label_create(hdr);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(name_lbl, bar_color, 0);

    char rem_str[10], max_str[10], val_buf[40];
    fmt_ms_short(rem_str, sizeof(rem_str), remaining_ms);
    fmt_ms_short(max_str, sizeof(max_str), max_ms);
    snprintf(val_buf, sizeof(val_buf), "%s/%s %d%%", rem_str, max_str, pct);

    lv_obj_t* val_lbl = lv_label_create(hdr);
    lv_label_set_text(val_lbl, val_buf);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(val_lbl, BR_COLOR_TEXT_SEC, 0);

    lv_obj_t* bar = lv_bar_create(col);
    lv_obj_set_size(bar, 296, 6);
    lv_obj_set_style_bg_color(bar, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_bg_color(bar, bar_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
}

static int32_t calc_monotonic_delta(uint32_t curr, uint32_t prev) {
    if (curr >= prev) {
        return (int32_t)(curr - prev);
    }
    return (int32_t)curr;
}

static int32_t calc_signed_delta(uint32_t curr, uint32_t prev) {
    return (int32_t)curr - (int32_t)prev;
}

static void fmt_counter_value(char* buf, size_t sz, uint32_t value, bool as_ms) {
    if (as_ms) {
        if (value >= 1000) {
            uint32_t whole = value / 1000;
            uint32_t tenths = (value % 1000) / 100;
            snprintf(buf, sz, "%" PRIu32 ".%" PRIu32 "s", whole, tenths);
        } else {
            snprintf(buf, sz, "%" PRIu32 "ms", value);
        }
    } else {
        snprintf(buf, sz, "%" PRIu32, value);
    }
}

static void create_counter_card(lv_obj_t* parent, uint32_t value, const char* label, int32_t delta,
                                bool show_delta, delta_polarity_t polarity, bool as_ms,
                                lv_color_t value_color) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, 96, 64);
    lv_obj_set_style_bg_color(card, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, BR_RADIUS, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char val_buf[20];
    fmt_counter_value(val_buf, sizeof(val_buf), value, as_ms);

    lv_obj_t* val_lbl = lv_label_create(card);
    lv_label_set_text(val_lbl, val_buf);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(val_lbl, value_color, 0);

    lv_obj_t* delta_lbl = lv_label_create(card);
    if (show_delta && delta != 0) {
        char delta_buf[20];
        snprintf(delta_buf, sizeof(delta_buf), "%+" PRId32, delta);
        lv_label_set_text(delta_lbl, delta_buf);

        bool positive = (delta > 0);
        bool good = (polarity == DELTA_GOOD_WHEN_UP) ? positive : !positive;
        lv_obj_set_style_text_color(delta_lbl, good ? BR_COLOR_PRIMARY : BR_COLOR_DANGER, 0);
    } else {
        lv_label_set_text(delta_lbl, "-");
        lv_obj_set_style_text_color(delta_lbl, BR_COLOR_TEXT_SEC, 0);
    }
    lv_obj_set_style_text_font(delta_lbl, &lv_font_montserrat_12, 0);

    lv_obj_t* name_lbl = lv_label_create(card);
    lv_label_set_text(name_lbl, label);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(name_lbl, BR_COLOR_TEXT_SEC, 0);
}

static void create_reach_row(lv_obj_t* parent, const char* name, int count, int total,
                             lv_color_t bar_color) {
    lv_obj_t* col = lv_obj_create(parent);
    lv_obj_set_size(col, 296, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    int pct = (total > 0) ? (count * 100) / total : 0;
    if (pct > 100)
        pct = 100;

    lv_obj_t* hdr = lv_obj_create(col);
    lv_obj_set_size(hdr, 296, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* name_lbl = lv_label_create(hdr);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(name_lbl, bar_color, 0);

    char val_buf[32];
    snprintf(val_buf, sizeof(val_buf), "%d nodes (%d%%)", count, pct);

    lv_obj_t* val_lbl = lv_label_create(hdr);
    lv_label_set_text(val_lbl, val_buf);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(val_lbl, BR_COLOR_TEXT_SEC, 0);

    lv_obj_t* bar = lv_bar_create(col);
    lv_obj_set_size(bar, 296, 6);
    lv_obj_set_style_bg_color(bar, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_bg_color(bar, bar_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
}

/* -------------------------------------------------------------------------
 * Traffic debug toggle (moved here from Settings: it controls the Traffic
 * Monitor row directly below, so it lives next to what it gates).
 * ------------------------------------------------------------------------- */
static void traffic_debug_changed_cb(lv_event_t* e) {
    lv_obj_t* sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    traffic_debug_t* td = mesh_get_traffic_debug();
    if (td) {
        traffic_debug_enable(td, on);
        ui_toast_show(on ? "Traffic debug on" : "Traffic debug off");
    }
}

/* -------------------------------------------------------------------------
 * Traffic Monitor navigation callback
 * ------------------------------------------------------------------------- */
/* The button lives in the content area scr_traffic_create cleans, so the
 * transition runs out of its own click via ui_zone_add_deferred_click. */
static void traffic_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    scr_traffic_create(layout);
}

static void traffic_open_async(void* arg) {
    bramble_layout_t* layout = (bramble_layout_t*)arg;
    if (!layout)
        return;
    lv_refr_now(lv_display_get_default());
    layout_rebuild_content(layout, traffic_builder, NULL);
}

void scr_stats_create(bramble_layout_t* layout) {
    /* Counters, the system block and the peer list overflow the viewport, and
     * the one focusable widget (Traffic Monitor) sits at the very bottom: it is
     * only reachable because this column scrolls. */
    lv_obj_t* cont = ui_zone_scroll_column(layout_get_content(layout));
    lv_obj_set_style_pad_all(cont, BR_PADDING, 0);
    lv_obj_set_style_pad_row(cont, 6, 0);

    const ui_mesh_state_t* state = ui_shared_mesh_state();

    routing_table_t routes;
    mesh_get_routes(&routes);

    uint32_t reach_addrs[MAX_NEIGHBORS + MAX_ROUTES];
    uint8_t reach_hops[MAX_NEIGHBORS + MAX_ROUTES];
    int reach_count = 0;

    for (int i = 0; i < state->neighbors.count && reach_count < (MAX_NEIGHBORS + MAX_ROUTES); i++) {
        const neighbor_entry_t* n = &state->neighbors.entries[i];
        reach_addrs[reach_count] = n->addr;
        reach_hops[reach_count] = 1;
        reach_count++;
    }

    for (int i = 0; i < routes.count; i++) {
        const route_entry_t* r = &routes.entries[i];
        if (r->state == ROUTE_BROKEN || r->state == ROUTE_STALE)
            continue;

        int found = -1;
        for (int j = 0; j < reach_count; j++) {
            if (reach_addrs[j] == r->dest_addr) {
                found = j;
                break;
            }
        }

        if (found >= 0) {
            if (r->hop_count > 0 && r->hop_count < reach_hops[found]) {
                reach_hops[found] = r->hop_count;
            }
            continue;
        }

        if (reach_count < (MAX_NEIGHBORS + MAX_ROUTES)) {
            reach_addrs[reach_count] = r->dest_addr;
            reach_hops[reach_count] = (r->hop_count > 0) ? r->hop_count : 1;
            reach_count++;
        }
    }

    int hop1_count = 0;
    int hop2_count = 0;
    int hop3plus_count = 0;
    for (int i = 0; i < reach_count; i++) {
        if (reach_hops[i] <= 1)
            hop1_count++;
        else if (reach_hops[i] == 2)
            hop2_count++;
        else
            hop3plus_count++;
    }

    traffic_debug_t* td = mesh_get_traffic_debug();
    uint32_t dropped = td ? traffic_debug_get_dropped(td) : 0;

    uint32_t air_used_ms = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t max_ms = state->airtime.max_ms[i];
        uint32_t remaining_ms = state->airtime.tokens_ms[i];
        if (max_ms > remaining_ms) {
            air_used_ms += (max_ms - remaining_ms);
        }
    }

    stats_snapshot_t curr = {
        .tx = state->packets_tx,
        .rx = state->packets_rx,
        .dropped = dropped,
        .neighbors = (uint32_t)state->neighbors.count,
        .routes = (uint32_t)route_count(&routes),
        .air_used_ms = air_used_ms,
    };

    bool show_delta = s_has_prev_snapshot;
    stats_delta_t delta = {0};
    if (show_delta) {
        delta.tx = calc_monotonic_delta(curr.tx, s_prev_snapshot.tx);
        delta.rx = calc_monotonic_delta(curr.rx, s_prev_snapshot.rx);
        delta.dropped = calc_monotonic_delta(curr.dropped, s_prev_snapshot.dropped);
        delta.neighbors = calc_signed_delta(curr.neighbors, s_prev_snapshot.neighbors);
        delta.routes = calc_signed_delta(curr.routes, s_prev_snapshot.routes);
        delta.air_used_ms = calc_signed_delta(curr.air_used_ms, s_prev_snapshot.air_used_ms);
    }
    s_prev_snapshot = curr;
    s_has_prev_snapshot = true;

    lv_obj_t* title = lv_label_create(cont);
    lv_label_set_text(title, "Stats");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);

    lv_obj_t* row1 = lv_obj_create(cont);
    lv_obj_set_size(row1, 304, 68);
    lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row1, 0, 0);
    lv_obj_set_style_pad_all(row1, 0, 0);
    lv_obj_clear_flag(row1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    create_counter_card(row1, curr.tx, "Sent", delta.tx, show_delta, DELTA_GOOD_WHEN_UP, false,
                        BR_COLOR_PRIMARY);
    create_counter_card(row1, curr.rx, "Received", delta.rx, show_delta, DELTA_GOOD_WHEN_UP, false,
                        BR_COLOR_PRIMARY);
    create_counter_card(row1, curr.dropped, "Dropped", delta.dropped, show_delta,
                        DELTA_GOOD_WHEN_DOWN, false,
                        curr.dropped > 0 ? BR_COLOR_DANGER : BR_COLOR_PRIMARY);

    lv_obj_t* row2 = lv_obj_create(cont);
    lv_obj_set_size(row2, 304, 68);
    lv_obj_set_style_bg_opa(row2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row2, 0, 0);
    lv_obj_set_style_pad_all(row2, 0, 0);
    lv_obj_clear_flag(row2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    create_counter_card(row2, curr.neighbors, "Neighbors", delta.neighbors, show_delta,
                        DELTA_GOOD_WHEN_UP, false, BR_COLOR_PRIMARY);
    create_counter_card(row2, curr.routes, "Routes", delta.routes, show_delta, DELTA_GOOD_WHEN_UP,
                        false, BR_COLOR_PRIMARY);
    create_counter_card(row2, curr.air_used_ms, "Air Used", delta.air_used_ms, show_delta,
                        DELTA_GOOD_WHEN_DOWN, true, BR_COLOR_WARNING);

    lv_obj_t* radio_lbl = lv_label_create(cont);
    char radio_buf[48];
    snprintf(radio_buf, sizeof(radio_buf), "Radio: %s  Last RSSI: %ddBm",
             state->radio_ok ? "OK" : "ERROR", state->last_rx_rssi);
    lv_label_set_text(radio_lbl, radio_buf);
    lv_obj_set_style_text_font(radio_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(radio_lbl, state->radio_ok ? BR_COLOR_TEXT_SEC : BR_COLOR_DANGER,
                                0);

    lv_obj_t* sep = lv_obj_create(cont);
    lv_obj_set_size(sep, 296, 1);
    lv_obj_set_style_bg_color(sep, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    lv_obj_t* sys_title = lv_label_create(cont);
    lv_label_set_text(sys_title, "System");
    lv_obj_set_style_text_font(sys_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sys_title, BR_COLOR_TEXT_SEC, 0);

    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t uptime_us = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t up_h = uptime_us / 3600;
    uint32_t up_m = (uptime_us % 3600) / 60;
    const char* ip = wifi_manager_get_ip();

    char sys_buf[160];
    if (ip && ip[0] != '\0') {
        snprintf(sys_buf, sizeof(sys_buf),
                 "Free heap:  %u KB\n"
                 "PSRAM free: %.1f MB\n"
                 "Uptime:     %luh %lum\n"
                 "IP address: %s",
                 (unsigned)(free_heap / 1024), free_psram / (1024.0 * 1024.0), (unsigned long)up_h,
                 (unsigned long)up_m, ip);
    } else {
        snprintf(sys_buf, sizeof(sys_buf),
                 "Free heap:  %u KB\n"
                 "PSRAM free: %.1f MB\n"
                 "Uptime:     %luh %lum\n"
                 "IP address: (WiFi off)",
                 (unsigned)(free_heap / 1024), free_psram / (1024.0 * 1024.0), (unsigned long)up_h,
                 (unsigned long)up_m);
    }

    lv_obj_t* sys_lbl = lv_label_create(cont);
    lv_label_set_text(sys_lbl, sys_buf);
    lv_obj_set_style_text_font(sys_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sys_lbl, BR_COLOR_TEXT, 0);

    lv_obj_t* sep2 = lv_obj_create(cont);
    lv_obj_set_size(sep2, 296, 1);
    lv_obj_set_style_bg_color(sep2, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep2, 0, 0);

    lv_obj_t* air_title = lv_label_create(cont);
    lv_label_set_text(air_title, "Airtime Budget");
    lv_obj_set_style_text_font(air_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(air_title, BR_COLOR_TEXT_SEC, 0);

    create_airtime_row(cont, "Critical", state->airtime.tokens_ms[1], state->airtime.max_ms[1],
                       BR_COLOR_CRITICAL);

    create_airtime_row(cont, "Normal", state->airtime.tokens_ms[0], state->airtime.max_ms[0],
                       BR_COLOR_PRIMARY);

    create_airtime_row(cont, "Broadcast", state->airtime.tokens_ms[2], state->airtime.max_ms[2],
                       BR_COLOR_WARNING);

    lv_obj_t* sep3 = lv_obj_create(cont);
    lv_obj_set_size(sep3, 296, 1);
    lv_obj_set_style_bg_color(sep3, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep3, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep3, 0, 0);

    lv_obj_t* reach_title = lv_label_create(cont);
    lv_label_set_text(reach_title, "Network Reach");
    lv_obj_set_style_text_font(reach_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(reach_title, BR_COLOR_TEXT_SEC, 0);

    char reach_total_buf[40];
    snprintf(reach_total_buf, sizeof(reach_total_buf), "Reachable nodes: %d", reach_count);
    lv_obj_t* reach_total = lv_label_create(cont);
    lv_label_set_text(reach_total, reach_total_buf);
    lv_obj_set_style_text_font(reach_total, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(reach_total, BR_COLOR_TEXT, 0);

    create_reach_row(cont, "1-hop", hop1_count, reach_count, BR_COLOR_SUCCESS);
    create_reach_row(cont, "2-hop", hop2_count, reach_count, BR_COLOR_PRIMARY);
    create_reach_row(cont, "3+ hop", hop3plus_count, reach_count, BR_COLOR_WARNING);

    ESP_LOGD(TAG,
             "stats deltas tx=%" PRId32 " rx=%" PRId32 " drop=%" PRId32 " peers=%" PRId32
             " routes=%" PRId32 " air=%" PRId32 " reach=%d",
             delta.tx, delta.rx, delta.dropped, delta.neighbors, delta.routes, delta.air_used_ms,
             reach_count);

    /* ---- Traffic Monitor link ---- */
    lv_obj_t* sep4 = lv_obj_create(cont);
    lv_obj_set_size(sep4, 296, 1);
    lv_obj_set_style_bg_color(sep4, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(sep4, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep4, 0, 0);

    /* Get event count for the button label (td already declared above) */
    bool td_on = td ? traffic_debug_is_enabled(td) : false;
    uint16_t td_cnt = td ? traffic_debug_get_count(td) : 0;

    lv_group_t* g = lv_group_get_default();

    /* Traffic debug toggle: gates the capture the Traffic Monitor below reads. */
    lv_obj_t* td_row = lv_obj_create(cont);
    lv_obj_set_size(td_row, 296, 28);
    lv_obj_set_style_bg_color(td_row, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(td_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(td_row, BR_RADIUS, 0);
    lv_obj_set_style_border_width(td_row, 0, 0);
    lv_obj_set_style_pad_all(td_row, 4, 0);
    lv_obj_clear_flag(td_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* td_lbl = lv_label_create(td_row);
    lv_label_set_text(td_lbl, "Traffic Debug");
    lv_obj_set_style_text_font(td_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(td_lbl, BR_COLOR_TEXT, 0);
    lv_obj_align(td_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* td_sw = lv_switch_create(td_row);
    lv_obj_set_size(td_sw, 40, 20);
    lv_obj_align(td_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(td_sw, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(td_sw, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(td_sw, BR_COLOR_TEXT, LV_PART_KNOB);
    if (td_on)
        lv_obj_add_state(td_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(td_sw, traffic_debug_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g)
        lv_group_add_obj(g, td_sw);

    char traffic_lbl_buf[40];
    if (td_on) {
        snprintf(traffic_lbl_buf, sizeof(traffic_lbl_buf),
                 LV_SYMBOL_LIST " Traffic Monitor  %u evts " LV_SYMBOL_RIGHT, td_cnt);
    } else {
        snprintf(traffic_lbl_buf, sizeof(traffic_lbl_buf),
                 LV_SYMBOL_LIST " Traffic Monitor  [off] " LV_SYMBOL_RIGHT);
    }

    lv_obj_t* traffic_btn = lv_btn_create(cont);
    lv_obj_set_size(traffic_btn, 296, 28);
    lv_obj_set_style_bg_color(traffic_btn, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(traffic_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(traffic_btn, BR_RADIUS, 0);
    lv_obj_set_style_border_width(traffic_btn, 0, 0);
    lv_obj_set_style_shadow_width(traffic_btn, 0, 0);
    lv_obj_set_style_pad_all(traffic_btn, 4, 0);
    ui_zone_add_deferred_click(traffic_btn, traffic_open_async, layout);

    lv_obj_t* traffic_lbl = lv_label_create(traffic_btn);
    lv_label_set_text(traffic_lbl, traffic_lbl_buf);
    lv_obj_set_style_text_font(traffic_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(traffic_lbl, BR_COLOR_TEXT, 0);
    lv_obj_center(traffic_lbl);

    if (g)
        lv_group_add_obj(g, traffic_btn);

    /* No ui_zone_reset_to_content() here: this builder runs only through
     * layout_rebuild_content (tab dispatch and the Traffic screen's Back
     * button), which owns the reset. */
}
