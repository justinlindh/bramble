#include "scr_settings.h"
#include "scr_settings_internal.h"
#include "theme/bramble_theme.h"
#include "ui_zone.h"
#include "lvgl.h"
#include <stdio.h>

/* ── Settings hub ────────────────────────────────────────────────────────
 *
 * The Settings tab is a hub of six rows, one per domain, each showing a live
 * value summary and opening a subpage. The old flat scroll (seven domains in one
 * 1700-line column) lives on unchanged inside the subpage builders; this file
 * owns only the hub and the two helpers every subpage shares. */

/* ── Shared: labeled setting row ─────────────────────────────────────────
 * The row every settings section builds. Verbatim from the pre-hub screen. */
lv_obj_t* settings_create_setting_row(lv_obj_t* parent, const char* label) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, 304, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(row, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, BR_RADIUS, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_30, LV_STATE_FOCUSED);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    return row;
}

/* ── Shared: subpage chrome + back navigation ───────────────────────────── */

/* Adapter so the hub can be a layout_rebuild_content builder (tab dispatch calls
 * scr_settings_create directly; back-navigation rebuilds through here). */
static void settings_hub_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    scr_settings_create(layout);
}

/* Back chevron lives in the content area the hub rebuild cleans, so it defers
 * out of its own click (see ui_defer / ui_zone_add_deferred_click). Rebuilding
 * the whole hub is deliberate: it re-reads every row summary from its live
 * source, so a value changed on a subpage shows updated on return. */
static void settings_back_to_hub_async(void* arg) {
    bramble_layout_t* layout = (bramble_layout_t*)arg;
    if (!layout)
        return;
    layout_rebuild_content(layout, settings_hub_builder, NULL);
}

lv_obj_t* settings_subpage_begin(bramble_layout_t* layout, const char* title) {
    /* content_area is a fixed canvas; a flex column lets the header pin to the
     * top while the scroll column below it grows to fill the rest. Same shape as
     * the Traffic screen, which keeps the tab bar and a chrome back button. */
    lv_obj_t* cont = layout_get_content(layout);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);

    lv_obj_t* header = lv_obj_create(cont);
    lv_obj_set_size(header, 320, 28);
    lv_obj_set_style_bg_color(header, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 48, 20);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back_btn, BR_RADIUS, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_pad_all(back_btn, 2, 0);
    lv_obj_t* back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Bk");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(back_lbl, BR_COLOR_TEXT, 0);
    lv_obj_center(back_lbl);
    ui_zone_add_deferred_click(back_btn, settings_back_to_hub_async, layout);

    /* Back is a header action (chrome); the tab bar stays visible. Registered
     * before layout_chrome_tabs_last so it heads the chrome ring and a
     * content->chrome hop lands on Back, not on the tab strip. */
    ui_zone_add_chrome(back_btn, true);

    lv_obj_t* title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_lbl, BR_COLOR_TEXT, 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, 0);

    layout_chrome_tabs_last(layout);

    lv_obj_t* col = ui_zone_scroll_column(cont);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_flex_grow(col, 1); /* fill the height left by the fixed header */
    lv_obj_set_style_pad_all(col, BR_PADDING, 0);
    lv_obj_set_style_pad_row(col, 4, 0);
    return col;
}

/* ── Hub rows ────────────────────────────────────────────────────────────
 * One deferred open per subpage: the row lives in the content area the subpage
 * rebuild cleans, so the transition runs from ui_defer after event dispatch. */
static void open_identity_async(void* arg) {
    layout_rebuild_content((bramble_layout_t*)arg, settings_identity_builder, NULL);
}
static void open_device_async(void* arg) {
    layout_rebuild_content((bramble_layout_t*)arg, settings_device_builder, NULL);
}
static void open_location_async(void* arg) {
    layout_rebuild_content((bramble_layout_t*)arg, settings_location_builder, NULL);
}
static void open_channels_async(void* arg) {
    layout_rebuild_content((bramble_layout_t*)arg, settings_channels_builder, NULL);
}
static void open_radio_async(void* arg) {
    layout_rebuild_content((bramble_layout_t*)arg, settings_radio_builder, NULL);
}
static void open_connectivity_async(void* arg) {
    layout_rebuild_content((bramble_layout_t*)arg, settings_connectivity_builder, NULL);
}

static void add_hub_row(lv_obj_t* cont, lv_group_t* g, bramble_layout_t* layout, const char* label,
                        const char* value, lv_async_cb_t open_async) {
    lv_obj_t* row = settings_create_setting_row(cont, label);

    lv_obj_t* val = lv_label_create(row);
    char buf[72];
    snprintf(buf, sizeof(buf), "%s  " LV_SYMBOL_RIGHT, value);
    lv_label_set_text(val, buf);
    lv_obj_set_style_text_color(val, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_12, 0);
    lv_obj_set_width(val, 196);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);

    ui_zone_add_deferred_click(row, open_async, layout);
    if (g)
        lv_group_add_obj(g, row);
}

/* ── Screen entry point (hub) ────────────────────────────────────────────── */

void scr_settings_create(bramble_layout_t* layout) {
    lv_obj_t* cont = ui_zone_scroll_column(layout_get_content(layout));
    lv_obj_set_style_pad_all(cont, BR_PADDING, 0);
    lv_obj_set_style_pad_row(cont, 4, 0);

    lv_group_t* g = lv_group_get_default();

    lv_obj_t* title = lv_label_create(cont);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);

    char sum[64];

    settings_identity_summary(sum, sizeof(sum));
    add_hub_row(cont, g, layout, "Identity", sum, open_identity_async);

    settings_device_summary(sum, sizeof(sum));
    add_hub_row(cont, g, layout, "Device", sum, open_device_async);

    settings_location_summary(sum, sizeof(sum));
    add_hub_row(cont, g, layout, "Location", sum, open_location_async);

    settings_channels_summary(sum, sizeof(sum));
    add_hub_row(cont, g, layout, "Channels", sum, open_channels_async);

    settings_radio_summary(sum, sizeof(sum));
    add_hub_row(cont, g, layout, "Radio", sum, open_radio_async);

    settings_connectivity_summary(sum, sizeof(sum));
    add_hub_row(cont, g, layout, "Connectivity", sum, open_connectivity_async);
}
