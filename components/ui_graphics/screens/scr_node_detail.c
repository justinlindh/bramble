#include "scr_node_detail.h"
#include "scr_nodes.h"
#include "scr_chat_messages.h"
#include "scr_map.h"
#include "theme/bramble_theme.h"
#include "node_detail_ui.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "scr_node_detail";

extern uint32_t mesh_send_location_packet(uint32_t dest_addr,
                                          const bramble_position_t *pos,
                                          uint8_t tier);
extern void mesh_get_location_state(location_manager_t *out);

static bramble_layout_t *s_layout = NULL;
static neighbor_entry_t s_neighbor;
static bool s_has_location = false;
static location_cache_entry_t s_location;
static uint32_t s_now_ms = 0;

static void back_click_cb(lv_event_t *e) {
    (void)e;
    if (!s_layout) return;
    lv_obj_clean(layout_get_content(s_layout));
    scr_nodes_create(s_layout);
}

static void dm_click_cb(lv_event_t *e) {
    (void)e;
    if (!s_layout || s_neighbor.addr == 0) return;
    scr_chat_messages_open_dm(s_layout, s_neighbor.addr);
}

static void map_click_cb(lv_event_t *e) {
    (void)e;
    if (!s_layout || s_neighbor.addr == 0) return;
    scr_map_set_focus_peer(s_neighbor.addr);
    layout_set_tab(s_layout, TAB_MAP);
}

static void share_loc_click_cb(lv_event_t *e) {
    (void)e;
    if (!s_layout || s_neighbor.addr == 0) return;

    static location_manager_t loc;
    mesh_get_location_state(&loc);
    if (!loc.my_position.valid) {
        ESP_LOGW(TAG, "No self location fix; cannot share to %08lX", (unsigned long)s_neighbor.addr);
        return;
    }

    uint32_t pkt = mesh_send_location_packet(s_neighbor.addr, &loc.my_position, LOCATION_TIER_COARSE);
    ESP_LOGI(TAG, "Shared coarse location with %08lX (pkt=%lu)",
             (unsigned long)s_neighbor.addr,
             (unsigned long)pkt);
}

static void add_action_btn(lv_obj_t *parent,
                           const char *text,
                           lv_event_cb_t cb,
                           bool focus) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 180, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(btn, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_radius(btn, BR_RADIUS, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, btn);
        if (focus) lv_group_focus_obj(btn);
    }
}

void scr_node_detail_open(bramble_layout_t *layout,
                          const neighbor_entry_t *neighbor,
                          bool has_location,
                          const location_cache_entry_t *location,
                          uint32_t now_ms) {
    if (!layout || !neighbor) return;

    s_layout = layout;
    s_neighbor = *neighbor;
    s_has_location = has_location;
    s_location = location ? *location : (location_cache_entry_t){0};
    s_now_ms = now_ms;

    lv_obj_t *cont = layout_get_content(layout);
    lv_obj_clean(cont);

    lv_obj_t *card = lv_obj_create(cont);
    lv_obj_set_size(card, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, BR_PADDING, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    char title[32];
    if (s_neighbor.name[0]) {
        snprintf(title, sizeof(title), "%s", s_neighbor.name);
    } else {
        snprintf(title, sizeof(title), "%08lX", (unsigned long)s_neighbor.addr);
    }

    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_lbl, BR_COLOR_TEXT, 0);

    lv_obj_t *sig_bar = lv_bar_create(card);
    lv_obj_set_size(sig_bar, 220, 8);
    lv_obj_set_style_bg_color(sig_bar, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sig_bar, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_bar_set_range(sig_bar, 0, 100);
    int pct = (s_neighbor.rssi + 120) * 100 / 70;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(sig_bar, pct, LV_ANIM_OFF);

    char signal_line[64];
    snprintf(signal_line, sizeof(signal_line), "RSSI %ddBm   SNR %d", s_neighbor.rssi, s_neighbor.snr);
    lv_obj_t *signal_lbl = lv_label_create(card);
    lv_label_set_text(signal_lbl, signal_line);
    lv_obj_set_style_text_font(signal_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(signal_lbl, BR_COLOR_TEXT_SEC, 0);

    char seen_line[40];
    node_detail_format_last_seen(seen_line, sizeof(seen_line), s_neighbor.last_heard, s_now_ms);
    lv_obj_t *seen_lbl = lv_label_create(card);
    lv_label_set_text(seen_lbl, seen_line);
    lv_obj_set_style_text_font(seen_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(seen_lbl, BR_COLOR_TEXT_SEC, 0);

    char loc_line[64];
    node_detail_format_location(loc_line,
                                sizeof(loc_line),
                                s_has_location,
                                s_location.pos.latitude_e7,
                                s_location.pos.longitude_e7,
                                s_location.received_ms,
                                s_now_ms);
    lv_obj_t *loc_lbl = lv_label_create(card);
    lv_label_set_text(loc_lbl, loc_line);
    lv_obj_set_width(loc_lbl, lv_pct(100));
    lv_label_set_long_mode(loc_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(loc_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(loc_lbl, BR_COLOR_TEXT_SEC, 0);

    lv_obj_t *actions = lv_obj_create(card);
    lv_obj_set_width(actions, lv_pct(100));
    lv_obj_set_height(actions, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_row(actions, 6, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    add_action_btn(actions, "DM", dm_click_cb, true);
    add_action_btn(actions, "Show on Map", map_click_cb, false);
    add_action_btn(actions, "Share My Location", share_loc_click_cb, false);
    add_action_btn(actions, "Back", back_click_cb, false);
}
