#include "scr_layout.h"
#include "ui_zone.h"
#include "ui_shared_state.h"
#include "scr_chat_list.h"
#include "scr_nodes.h"
#include "scr_map.h"
#include "scr_stats.h"
#include "scr_settings.h"
#include "theme/bramble_theme.h"
#include "battery.h"
#include "board_config.h"
#include "gps.h"
#include "bramble_tz.h"
#include "tz_store.h"
#include "gnss_status.h"
#include "routing.h"
#include "airtime_budget.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "layout";
static bramble_layout_t s_layout;

extern const char* mesh_get_node_name(void);
extern bool bramble_gps_enabled(void); /* persisted GPS power preference (main.c) */

static const char* tab_labels[TAB_COUNT] = {LV_SYMBOL_ENVELOPE " Chat", LV_SYMBOL_WIFI " Nodes",
                                            LV_SYMBOL_GPS " Map", LV_SYMBOL_BARS " Stats",
                                            LV_SYMBOL_SETTINGS " Set"};

/* Carries the requested tab from the click dispatch to the deferred switch.
 * A single slot is correct: taps that arrive before the deferred call runs
 * are the same user resolving on one destination, and the last one wins.
 * Two taps in one dispatch pass queue two switches that both read the final
 * value, so the second is a redundant rebuild rather than a wrong one. */
static bramble_tab_t s_pending_tab;

static void tab_switch_async(void* arg) {
    (void)arg;
    layout_set_tab(&s_layout, s_pending_tab);
}

static void tab_click_cb(lv_event_t* e) {
    bramble_tab_t tab = (bramble_tab_t)(intptr_t)lv_event_get_user_data(e);
    if (tab == TAB_MAP)
        scr_map_clear_focus_peer(); /* direct visits start unfocused */

    /* Defer the switch out of this CLICKED dispatch. layout_set_tab runs
     * lv_refr_now and then lv_obj_clean on the content area, so performing it
     * here re-enters the renderer and frees widgets while lv_event_send is
     * still iterating the event array it captured before invoking this
     * callback, and while event_send_core is walking the parent bubble chain.
     * The chat bubble handler defers for the same reason. */
    s_pending_tab = tab;
    ui_defer(tab_switch_async, NULL);
}

void layout_rebuild_content(bramble_layout_t* layout, void (*builder)(bramble_layout_t*, void*),
                            void* ctx) {
    if (!layout || !builder)
        return;
    lv_obj_clean(layout->content_area);
    builder(layout, ctx);
    /* A fresh screen always starts focused in its content zone. Owning this here
     * (not in each builder) is the point: no builder can forget it. */
    ui_zone_reset_to_content();
}

/* Dispatch to the active tab's builder. Runs from layout_rebuild_content after
 * the content area is cleaned; layout->active_tab is set before the rebuild. */
static void tab_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    switch (layout->active_tab) {
    case TAB_CHAT:
        scr_chat_list_create(layout);
        break;
    case TAB_NODES:
        scr_nodes_create(layout);
        break;
    case TAB_MAP:
        scr_map_create(layout);
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

bramble_layout_t* layout_create(void) {
    lv_obj_t* scr = lv_screen_active();
    s_layout.screen = scr;
    lv_obj_set_style_bg_color(scr, BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* === Status Bar (top 20px) === */
    s_layout.status_bar = lv_obj_create(scr);
    lv_obj_set_size(s_layout.status_bar, 320, BR_STATUS_BAR_H);
    lv_obj_set_pos(s_layout.status_bar, 0, 0);
    lv_obj_set_style_bg_color(s_layout.status_bar, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_layout.status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_layout.status_bar, 0, 0);
    lv_obj_set_style_border_width(s_layout.status_bar, 0, 0);
    lv_obj_set_style_pad_all(s_layout.status_bar, 2, 0);
    lv_obj_clear_flag(s_layout.status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_layout.status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_layout.status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_layout.lbl_battery = lv_label_create(s_layout.status_bar);
    lv_label_set_text(s_layout.lbl_battery, LV_SYMBOL_BATTERY_FULL " 100%");
    lv_obj_set_style_text_font(s_layout.lbl_battery, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_layout.lbl_battery, BR_COLOR_TEXT, 0);

    s_layout.lbl_signal = lv_label_create(s_layout.status_bar);
    lv_label_set_text(s_layout.lbl_signal, LV_SYMBOL_WIFI " 0");
    lv_obj_set_style_text_font(s_layout.lbl_signal, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_layout.lbl_signal, BR_COLOR_TEXT, 0);

    /* GNSS is an icon plus a two-character count: the color carries the
     * three-way state and the count carries how many satellites are behind it
     * (see layout_update_status). Created at its full width so the
     * SPACE_BETWEEN row does not reflow on the first update. */
    s_layout.lbl_gps = lv_label_create(s_layout.status_bar);
    lv_label_set_text(s_layout.lbl_gps, LV_SYMBOL_GPS " --");
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
    /* content_area is a fixed canvas: every screen positions header/list/
     * compose-bar children by absolute coordinates and owns its own scrolling
     * child (message list, form, etc). LVGL objects are scrollable by
     * default, so without this a child's scroll-chain (e.g. the chat
     * message list reaching its end) drags content_area itself and carries
     * "fixed" elements like the compose bar off screen. */
    lv_obj_clear_flag(s_layout.content_area, LV_OBJ_FLAG_SCROLLABLE);

    /* === Tab Bar (bottom 40px) === */
    s_layout.tab_bar = lv_obj_create(scr);
    lv_obj_set_size(s_layout.tab_bar, 320, BR_TAB_BAR_H);
    lv_obj_set_pos(s_layout.tab_bar, 0, 240 - BR_TAB_BAR_H);
    lv_obj_set_style_bg_color(s_layout.tab_bar, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_layout.tab_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_layout.tab_bar, 0, 0);
    lv_obj_set_style_border_width(s_layout.tab_bar, 0, 0);
    lv_obj_set_style_pad_all(s_layout.tab_bar, 0, 0);
    lv_obj_clear_flag(s_layout.tab_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_layout.tab_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_layout.tab_bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t* btn = lv_btn_create(s_layout.tab_bar);
        lv_obj_set_size(btn, 60, 36);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 4, 0);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, tab_labels[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, tab_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        s_layout.tab_btns[i] = btn;
        /* Bottom nav is chrome, not content. Default lands on layout_set_tab,
         * once the initial active tab is known. */
        ui_zone_add_chrome(btn, false);
    }

    s_layout.active_tab = TAB_CHAT;
    layout_set_tab(&s_layout, TAB_CHAT);

    ESP_LOGI(TAG, "Layout created");
    return &s_layout;
}

void layout_set_tab(bramble_layout_t* layout, bramble_tab_t tab) {
    if (tab >= TAB_COUNT)
        return;

    /* Any tab switch means we are back on a full tab screen: a full-screen
     * view (compose, channel create, DM) may have hidden the bar and pulled
     * its buttons from the input group. Always restore both, so an incoming
     * message that forces TAB_CHAT never strands the user with no tabs. */
    layout_set_tab_bar_hidden(layout, false);

    for (int i = 0; i < TAB_COUNT; i++) {
        if (i == (int)tab) {
            lv_obj_set_style_bg_color(layout->tab_btns[i], BR_COLOR_PRIMARY, 0);
            lv_obj_set_style_bg_opa(layout->tab_btns[i], LV_OPA_30, 0);
            /* Where a content->chrome hop lands: the current tab. */
            ui_zone_set_chrome_default(layout->tab_btns[i]);
        } else {
            lv_obj_set_style_bg_opa(layout->tab_btns[i], LV_OPA_TRANSP, 0);
        }
    }

    layout->active_tab = tab;

    /* Clear unread badge when switching to Chat tab */
    if (tab == TAB_CHAT) {
        extern void ui_graphics_clear_unread(void);
        ui_graphics_clear_unread();
        layout_set_unread(layout, 0);
    }

    /* Flex/content-size screens can leave pending layout tasks; flush before the
     * rebuild's clean. */
    lv_refr_now(lv_display_get_default());
    layout_rebuild_content(layout, tab_builder, NULL);
}

/* Pixels the node name may occupy in the 320 px status bar. What is left after
 * the battery (~49 px), signal (~30 px), GNSS badge (~27 px) and clock
 * (~31 px) items and the gaps a SPACE_BETWEEN row needs between them. */
#define BR_STATUS_NAME_MAX_W 150

/* Copy the longest prefix of src that fits max_w pixels at montserrat_12,
 * marking a cut with a trailing "..". Measuring beats counting characters: the
 * font is proportional, so "WWWWWWWW" is more than twice the width of
 * "iiiiiiii". Whole UTF-8 sequences only, so a cut never splits a codepoint. */
static void fit_to_width(char* out, size_t out_len, const char* src, int32_t max_w) {
    static const char kEllipsis[] = "..";
    if (!out || out_len < sizeof(kEllipsis) + 1)
        return;
    out[0] = '\0';
    if (!src)
        return;

    const lv_font_t* font = &lv_font_montserrat_12;
    size_t len = strlen(src);
    if (len < out_len && lv_text_get_width(src, (uint32_t)len, font, 0) <= max_w) {
        memcpy(out, src, len + 1);
        return;
    }

    /* Longest whole-codepoint prefix that leaves room for the marker, bounded
     * by the output buffer as well as by the pixel budget. */
    int32_t budget = max_w - lv_text_get_width(kEllipsis, sizeof(kEllipsis) - 1, font, 0);
    size_t limit = out_len - sizeof(kEllipsis);
    if (limit > len)
        limit = len;
    while (limit > 0 && ((unsigned char)src[limit] & 0xC0) == 0x80)
        limit--; /* never let the buffer bound itself land mid-codepoint */
    size_t keep = 0;
    for (size_t i = 0; i < limit;) {
        size_t next = i + 1;
        while (next < limit && ((unsigned char)src[next] & 0xC0) == 0x80)
            next++; /* skip UTF-8 continuation bytes */
        if (lv_text_get_width(src, (uint32_t)next, font, 0) > budget)
            break;
        keep = next;
        i = next;
    }
    memcpy(out, src, keep);
    memcpy(out + keep, kEllipsis, sizeof(kEllipsis));
}

void layout_update_status(bramble_layout_t* layout) {
    /* Battery */
    int pct = battery_read_pct();
    char buf[32];
    const char* batt_sym = pct > 75   ? LV_SYMBOL_BATTERY_FULL
                           : pct > 50 ? LV_SYMBOL_BATTERY_3
                           : pct > 25 ? LV_SYMBOL_BATTERY_2
                                      : LV_SYMBOL_BATTERY_1;
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
    const ui_mesh_state_t* state = ui_shared_mesh_state();

    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %d", state->neighbors.count);
    lv_label_set_text(layout->lbl_signal, buf);

    /* Clock, rendered in the zone configured in Settings (default UTC).
     *
     * A GPS fix is the only wall clock a node has: it supplies both the UTC
     * date (RMC) and the time of day (GGA), and the date is what makes a
     * daylight-saving rule evaluable. UTC stays the internal source of truth
     * and the zone is applied here, at render time.
     *
     * Mesh network time is deliberately not a fallback. timesync's "network
     * time" is local uptime plus a corroborated offset that no node ever
     * seeds from a real epoch, so it carries no wall-clock meaning (see the
     * firmware-reality note in docs/bramble-protocol-spec.md). Rendering it
     * as HH:MM would show a plausible but fabricated clock, which is worse
     * than showing no clock, so --:-- stands whenever GPS UTC is unavailable
     * or does not convert. */
    uint8_t gps_h, gps_m, gps_mo, gps_d;
    uint16_t gps_y;
    bool clock_shown = false;

    if (board_has_cap(BOARD_CAP_GPS) && gps_get_utc_hm(&gps_h, &gps_m) &&
        gps_get_utc_date(&gps_y, &gps_mo, &gps_d)) {
        char tz_spec[BRAMBLE_TZ_SPEC_MAX];
        tz_store_get(tz_spec, sizeof(tz_spec));

        const bramble_tz_time_t utc = {
            .year = gps_y, .month = gps_mo, .day = gps_d, .hour = gps_h, .minute = gps_m};
        bramble_tz_time_t local;
        bramble_tz_status_t st = bramble_tz_localtime(tz_spec, &utc, &local, NULL, 0);
        if (st == BRAMBLE_TZ_STD || st == BRAMBLE_TZ_DST) {
            snprintf(buf, sizeof(buf), "%02u:%02u", local.hour, local.minute);
            lv_label_set_text(layout->lbl_time, buf);
            clock_shown = true;
        }
    }

    if (!clock_shown) {
        lv_label_set_text(layout->lbl_time, "--:--");
    }

    /* GNSS: the icon color and the count together carry the three-way state,
     * because "no fix" alone cannot tell a receiver hearing nothing from one
     * hearing satellites and still converging.
     *   absent or powered off -> hidden
     *   no signal             -> danger red, "--"
     *   acquiring             -> warning amber, satellites tracked
     *   fix                   -> success green, satellites used
     * The count is right-aligned to a fixed two characters so the
     * SPACE_BETWEEN row does not shuffle every tick. */
    gnss_ui_input_t gnss;
    ui_shared_gnss_state(&gnss);
    gnss_ui_state_t gstate = gnss_ui_classify(&gnss);
    if (gstate == GNSS_UI_ABSENT) {
        lv_obj_add_flag(layout->lbl_gps, LV_OBJ_FLAG_HIDDEN);
    } else {
        char count[4];
        gnss_ui_badge_count(&gnss, count, sizeof(count));
        snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " %s", count);
        lv_label_set_text(layout->lbl_gps, buf);
        lv_obj_set_style_text_color(layout->lbl_gps,
                                    gstate == GNSS_UI_FIX         ? BR_COLOR_SUCCESS
                                    : gstate == GNSS_UI_ACQUIRING ? BR_COLOR_WARNING
                                                                  : BR_COLOR_DANGER,
                                    0);
        lv_obj_clear_flag(layout->lbl_gps, LV_OBJ_FLAG_HIDDEN);
    }

    /* Node name from the mesh (settable via Settings / bramble.setNodeName),
     * trimmed to the pixel budget the other status-bar items leave it. Names
     * run to BRAMBLE_NODE_NAME_MAX characters, which is wider than a 320 px
     * row can hold, and this is the last item in a SPACE_BETWEEN flow: without
     * the trim a long name walks the battery, signal and GNSS items off the
     * left edge. */
    const char* name = mesh_get_node_name();
    if (!name || !name[0])
        name = "BRAMBLE";
    char name_buf[40];
    fit_to_width(name_buf, sizeof(name_buf), name, BR_STATUS_NAME_MAX_W);
    lv_label_set_text(layout->lbl_node_name, name_buf);
}

void layout_set_unread(bramble_layout_t* layout, int count) {
    if (count > 0) {
        /* Create or update badge */
        if (layout->chat_badge == NULL) {
            /* Create badge container */
            layout->chat_badge = lv_obj_create(layout->tab_btns[TAB_CHAT]);
            lv_obj_set_size(layout->chat_badge, 20, 20);
            lv_obj_set_style_bg_color(layout->chat_badge, BR_COLOR_DANGER, 0);
            lv_obj_set_style_bg_opa(layout->chat_badge, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(layout->chat_badge, 10, 0); /* Circle */
            lv_obj_set_style_border_width(layout->chat_badge, 0, 0);
            lv_obj_set_style_pad_all(layout->chat_badge, 0, 0);
            lv_obj_align(layout->chat_badge, LV_ALIGN_TOP_RIGHT, -2, 2);
            lv_obj_clear_flag(layout->chat_badge, LV_OBJ_FLAG_SCROLLABLE);

            /* Add label with count */
            lv_obj_t* lbl = lv_label_create(layout->chat_badge);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
            lv_obj_center(lbl);
        }

        /* Update count text */
        lv_obj_t* lbl = lv_obj_get_child(layout->chat_badge, 0);
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

void layout_chrome_tabs_last(bramble_layout_t* layout) {
    /* Chrome ring order: the screen's header actions FIRST, the five nav tabs
     * after them. The tabs join the chrome group once at layout_create, so they
     * sit at the head of the ring and a screen's header actions land behind all
     * five: reaching "+Ch" from the Messages list took seven right-presses.
     * Re-adding a tab moves it to the tail, so calling this after a builder has
     * registered its header actions puts those actions first, and a
     * content->chrome hop lands on the screen's primary action with the tabs
     * still reachable rightward.
     *
     * A full-screen view (chat, compose, channel create) hides the tab bar,
     * which pulls the tabs out of the group entirely; nothing to reorder. */
    if (!layout || !layout->tab_bar || lv_obj_has_flag(layout->tab_bar, LV_OBJ_FLAG_HIDDEN))
        return;

    lv_group_t* g = ui_zone_chrome_group();
    if (!g)
        return;

    for (int i = 0; i < TAB_COUNT; i++) {
        if (layout->tab_btns[i]) {
            lv_group_remove_obj(layout->tab_btns[i]);
            lv_group_add_obj(g, layout->tab_btns[i]);
        }
    }
}

lv_obj_t* layout_get_content(bramble_layout_t* layout) { return layout->content_area; }

void layout_set_tab_bar_hidden(bramble_layout_t* layout, bool hidden) {
    /* Idempotent: re-adding buttons already in the group would reorder
     * focus on every tab switch. */
    bool already_hidden = lv_obj_has_flag(layout->tab_bar, LV_OBJ_FLAG_HIDDEN);
    if (hidden == already_hidden)
        return;

    lv_group_t* g = ui_zone_chrome_group();
    if (hidden) {
        lv_obj_add_flag(layout->tab_bar, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < TAB_COUNT; i++) {
            if (layout->tab_btns[i])
                lv_group_remove_obj(layout->tab_btns[i]);
        }
    } else {
        lv_obj_clear_flag(layout->tab_bar, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < TAB_COUNT; i++) {
            if (g && layout->tab_btns[i])
                lv_group_add_obj(g, layout->tab_btns[i]);
        }
    }
}
