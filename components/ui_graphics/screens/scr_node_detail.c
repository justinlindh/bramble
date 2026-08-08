#include "scr_node_detail.h"
#include "scr_nodes.h"
#include "scr_chat_messages.h"
#include "scr_map.h"
#include "ui_zone.h"
#include "ui_shared_state.h"
#include "node_presence.h"
#include "theme/bramble_theme.h"
#include "ui_toast.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "scr_node_detail";

/* Detail-card line formatters. Only this screen renders them, so they live
 * here as file-local helpers rather than in a shared header; the age
 * arithmetic and the age string come from node_presence, shared with the
 * Nodes list so both surfaces read the same and are host-tested once. */
static void node_detail_format_last_seen(char* out, size_t out_len, uint32_t last_seen_ms,
                                         uint32_t now_ms) {
    if (!out || out_len == 0)
        return;

    char age_buf[16];
    node_format_age(node_age_seconds(now_ms, last_seen_ms), age_buf, sizeof(age_buf));
    snprintf(out, out_len, "Last seen %s ago", age_buf);
}

static void node_detail_format_location(char* out, size_t out_len, bool has_location,
                                        int32_t latitude_e7, int32_t longitude_e7,
                                        uint32_t received_ms, uint32_t now_ms, bool age_known) {
    if (!out || out_len == 0)
        return;

    if (!has_location) {
        snprintf(out, out_len, "No location shared");
        return;
    }

    double lat = ((double)latitude_e7) / 1e7;
    double lon = ((double)longitude_e7) / 1e7;
    if (!age_known) {
        /* Restored from flash: the receipt time belongs to an earlier boot's
         * uptime clock, so subtracting it from this one's yields a number with
         * no meaning. It used to fall through to age_s == 0 and label a
         * position of any age "(now)". */
        snprintf(out, out_len, "%.6f, %.6f (last known)", lat, lon);
        return;
    }
    char age_buf[16];
    node_format_age(node_age_seconds(now_ms, received_ms), age_buf, sizeof(age_buf));
    snprintf(out, out_len, "%.6f, %.6f (%s ago)", lat, lon, age_buf);
}

extern uint32_t mesh_send_location_packet(uint32_t dest_addr, const bramble_position_t* pos,
                                          uint8_t tier);
extern void mesh_get_location_state(location_manager_t* out);
extern bool mesh_get_neighbor(uint32_t addr, neighbor_entry_t* out);

static bramble_layout_t* s_layout = NULL;
static neighbor_entry_t s_neighbor;
static bool s_has_location = false;
static location_cache_entry_t s_location;

/* Live-refresh state, valid only while the card exists. Everything on this card
 * is a view of a moving target: without the tick below, the age lines and the
 * signal readout froze at the values they had when the card was opened and
 * stayed there for as long as it was on screen. The widgets are created and
 * cleared together, so they live in one struct rather than four statics whose
 * lockstep a reader would have to prove.
 *
 * last_* hold what was last written to each label. LVGL's label setters do no
 * value comparison: every call frees and reallocates the string and invalidates
 * the area twice, whether or not the text changed. RSSI and SNR only move when
 * a frame is actually heard from the peer, which absent traffic is bounded by
 * the 60 s beacon interval, so at 1 Hz roughly 59 of every 60 writes would be
 * byte-identical repaints. */
static struct {
    lv_obj_t* signal_lbl;
    lv_obj_t* sig_bar;
    lv_obj_t* seen_lbl;
    lv_obj_t* loc_lbl;
    char last_seen[48];
    char last_loc[72];
    char last_signal[48];
} s_card;

static void set_label_if_changed(lv_obj_t* lbl, char* last, size_t last_len, const char* text) {
    if (strncmp(last, text, last_len) == 0)
        return;
    snprintf(last, last_len, "%s", text);
    lv_label_set_text(lbl, text);
}

/* Pull the peer and its shared location from live state, then repaint whatever
 * actually changed. Also the first-frame renderer, so the card cannot drift
 * from what the tick produces a second later. A peer purged out of the neighbor
 * table while its card is open keeps its last known signal values, and its age
 * simply keeps growing off the last_heard already held. */
static void node_detail_render(void) {
    if (!s_card.seen_lbl)
        return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    mesh_get_neighbor(s_neighbor.addr, &s_neighbor);

    const location_manager_t* loc = ui_shared_location_state();
    const location_cache_entry_t* entry = location_cache_get(loc, s_neighbor.addr);
    s_has_location = (entry != NULL);
    if (entry)
        s_location = *entry;

    char line[72];
    node_detail_format_last_seen(line, sizeof(line), s_neighbor.last_heard, now_ms);
    set_label_if_changed(s_card.seen_lbl, s_card.last_seen, sizeof(s_card.last_seen), line);

    snprintf(line, sizeof(line), "RSSI %ddBm   SNR %d", s_neighbor.rssi, s_neighbor.snr);
    set_label_if_changed(s_card.signal_lbl, s_card.last_signal, sizeof(s_card.last_signal), line);
    lv_bar_set_value(s_card.sig_bar, node_signal_pct(s_neighbor.rssi), LV_ANIM_OFF);

    node_detail_format_location(line, sizeof(line), s_has_location, s_location.pos.latitude_e7,
                                s_location.pos.longitude_e7, s_location.received_ms, now_ms,
                                s_location.age_known);
    set_label_if_changed(s_card.loc_lbl, s_card.last_loc, sizeof(s_card.last_loc), line);
}

static void node_detail_refresh_cb(lv_timer_t* timer) {
    (void)timer;
    node_detail_render();
}

static void node_detail_delete_cb(lv_event_t* e) {
    lv_timer_t* timer = (lv_timer_t*)lv_event_get_user_data(e);
    if (timer)
        lv_timer_delete(timer);
    memset(&s_card, 0, sizeof(s_card));
}

/* This screen's action buttons live in the content area that each destination
 * cleans, so every one of them defers out of its own click. See ui_defer.
 * (Share My Location rebuilds nothing and stays inline.) */
static void nodes_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    scr_nodes_create(layout);
}

static void back_to_nodes_async(void* arg) {
    (void)arg;
    if (!s_layout)
        return;
    layout_rebuild_content(s_layout, nodes_builder, NULL);
}

static void back_click_cb(lv_event_t* e) {
    (void)e;
    if (!s_layout)
        return;
    ui_defer(back_to_nodes_async, NULL);
}

static void open_dm_async(void* arg) {
    (void)arg;
    if (!s_layout || s_neighbor.addr == 0)
        return;
    scr_chat_messages_open_dm(s_layout, s_neighbor.addr);
}

static void dm_click_cb(lv_event_t* e) {
    (void)e;
    if (!s_layout || s_neighbor.addr == 0)
        return;
    ui_defer(open_dm_async, NULL);
}

static void show_on_map_async(void* arg) {
    (void)arg;
    if (!s_layout)
        return;
    layout_set_tab(s_layout, TAB_MAP);
}

static void map_click_cb(lv_event_t* e) {
    (void)e;
    if (!s_layout || s_neighbor.addr == 0)
        return;
    /* Record the peer inline (it is read by the map builder); only the tab
     * switch, which cleans this button away, is deferred. */
    scr_map_set_focus_peer(s_neighbor.addr);
    ui_defer(show_on_map_async, NULL);
}

static void share_loc_click_cb(lv_event_t* e) {
    (void)e;
    if (!s_layout || s_neighbor.addr == 0)
        return;

    static location_manager_t loc;
    mesh_get_location_state(&loc);
    if (!loc.my_position.valid) {
        ESP_LOGW(TAG, "No self location fix; cannot share to %08lX",
                 (unsigned long)s_neighbor.addr);
        ui_toast_show("No position to share yet");
        return;
    }

    uint32_t pkt =
        mesh_send_location_packet(s_neighbor.addr, &loc.my_position, LOCATION_TIER_COARSE);
    ESP_LOGI(TAG, "Shared coarse location with %08lX (pkt=%lu)", (unsigned long)s_neighbor.addr,
             (unsigned long)pkt);
    ui_toast_show("Location shared");
}

/* One compact action per column of a single horizontal row. grow 0 keeps a
 * fixed-width button (the Back arrow); grow 1 shares the remaining width
 * equally. Height stays at the touch-target minimum: the T-Deck has a
 * touchscreen, so the buttons shrank in width and count, not in tap size. */
static void add_action_btn(lv_obj_t* parent, const char* text, lv_event_cb_t cb, bool focus,
                           uint8_t grow) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_height(btn, BR_TAP_TARGET_MIN);
    if (grow) {
        lv_obj_set_flex_grow(btn, grow);
    } else {
        lv_obj_set_width(btn, BR_TAP_TARGET_MIN);
    }
    lv_obj_set_style_bg_color(btn, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_radius(btn, BR_RADIUS, 0);
    lv_obj_set_style_pad_all(btn, 2, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, btn);
        if (focus)
            lv_group_focus_obj(btn);
    }
}

void scr_node_detail_open(bramble_layout_t* layout, const neighbor_entry_t* neighbor) {
    if (!layout || !neighbor)
        return;

    s_layout = layout;
    s_neighbor = *neighbor;
    s_has_location = false;
    s_location = (location_cache_entry_t){0};
    memset(&s_card, 0, sizeof(s_card));

    /* Builder: runs through layout_rebuild_content (from node_open_async and the
     * Back button), which owns the content-area clean and the trailing zone
     * reset. */
    lv_obj_t* cont = layout_get_content(layout);

    lv_obj_t* card = lv_obj_create(cont);
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

    lv_obj_t* title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_lbl, BR_COLOR_TEXT, 0);

    /* The four live widgets are created empty and filled by node_detail_render
     * below, so the first frame and every tick after it come from one piece of
     * code and cannot drift apart. */
    s_card.sig_bar = lv_bar_create(card);
    lv_obj_set_size(s_card.sig_bar, 220, 8);
    lv_obj_set_style_bg_color(s_card.sig_bar, BR_COLOR_SURFACE_2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_card.sig_bar, BR_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_bar_set_range(s_card.sig_bar, 0, 100);

    s_card.signal_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_card.signal_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_card.signal_lbl, BR_COLOR_TEXT_SEC, 0);

    s_card.seen_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_card.seen_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_card.seen_lbl, BR_COLOR_TEXT_SEC, 0);

    s_card.loc_lbl = lv_label_create(card);
    lv_obj_set_width(s_card.loc_lbl, lv_pct(100));
    lv_label_set_long_mode(s_card.loc_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_card.loc_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_card.loc_lbl, BR_COLOR_TEXT_SEC, 0);

    node_detail_render();

    /* Live refresh every second, matching the Nodes list: the ages count up
     * and the signal readout follows the peer instead of showing whatever was
     * true at the moment the card was opened. The timer is owned by the card,
     * so it stops the moment this screen is navigated away from. */
    lv_timer_t* refresh = lv_timer_create(node_detail_refresh_cb, 1000, NULL);
    lv_obj_add_event_cb(card, node_detail_delete_cb, LV_EVENT_DELETE, refresh);

    /* Actions: ONE horizontal row instead of the old column of four
     * screen-wide buttons. The stack forced this screen to scroll (and put
     * the top button flush against the clip edge); a single row of compact
     * buttons fits the whole screen without scrolling. Back is a fixed-width
     * arrow (leftmost, mirroring where back lives in the chrome), the three
     * real actions share the rest of the width. */
    lv_obj_t* actions = lv_obj_create(card);
    lv_obj_set_width(actions, lv_pct(100));
    lv_obj_set_height(actions, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_column(actions, 6, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    add_action_btn(actions, LV_SYMBOL_LEFT, back_click_cb, false, 0);
    add_action_btn(actions, "DM", dm_click_cb, true, 1);
    add_action_btn(actions, "Map", map_click_cb, false, 1);
    add_action_btn(actions, "Share", share_loc_click_cb, false, 1);
}
