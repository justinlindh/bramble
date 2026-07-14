#include "scr_settings_internal.h"
#include "theme/bramble_theme.h"
#include "ui_confirm.h"
#include "ui_focus.h"
#include "ui_toast.h"
#include "ui_zone.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "scr_set_ch";

extern int mesh_add_channel(const char* name, const uint8_t* psk, size_t psk_len);
extern int mesh_remove_channel(int index);
extern int mesh_get_channel_count(void);
extern const char* mesh_get_channel_name(int index);
extern int mesh_get_channel_security(int index, bool* has_psk, uint16_t* epoch);
extern int mesh_set_default_channel(int index);
extern int mesh_get_channel_info(int* default_idx);

/* Forward declare so callbacks can rebuild the section */
static void build_channel_manager_section(lv_obj_t* cont, lv_group_t* g);

/* ui_zone_track'd, like every settings widget cached here: the deferred
 * add/remove/default callbacks below re-enter through this pointer, and a tab
 * switch (an incoming message forces TAB_CHAT) cleans the content area out from
 * under it. Tracked, the NULL guard in channel_refresh_list is real; untracked
 * it just read freed memory that happened to be non-NULL. */
static lv_obj_t* s_channel_section_cont = NULL;
/* Not a widget: the long-lived content group, safe to cache raw. */
static lv_group_t* s_channel_group = NULL;

/* Add channel modal */
static lv_obj_t* s_ch_add_overlay = NULL;
static lv_obj_t* s_ch_name_ta = NULL;
static lv_obj_t* s_ch_psk_ta = NULL;

static void channel_add_close(void) {
    if (s_ch_add_overlay) {
        ui_focus_pop_modal();
        /* Called from Cancel/Save INSIDE this overlay; defer the delete so the
         * clicked button survives its own event dispatch (see
         * identity_qr_close). */
        lv_obj_delete_async(s_ch_add_overlay);
        s_ch_add_overlay = NULL;
        s_ch_name_ta = NULL;
        s_ch_psk_ta = NULL;
    }
}

static void channel_refresh_list(void);

static void channel_add_save_cb(lv_event_t* e) {
    (void)e;
    if (!s_ch_name_ta)
        return;
    const char* name = lv_textarea_get_text(s_ch_name_ta);
    const char* psk = s_ch_psk_ta ? lv_textarea_get_text(s_ch_psk_ta) : NULL;

    if (!name || name[0] == '\0') {
        ui_toast_show("Channel name required");
        return; /* keep the modal open for correction */
    }

    size_t psk_len = (psk && psk[0]) ? strlen(psk) : 0;
    int idx = mesh_add_channel(name, psk_len > 0 ? (const uint8_t*)psk : NULL, psk_len);
    if (idx >= 0) {
        ESP_LOGI(TAG, "Channel '%s' added at index %d", name, idx);
        ui_toast_show("Channel created");
    } else {
        ESP_LOGE(TAG, "Failed to add channel '%s'", name);
        ui_toast_show("Failed to add channel");
    }
    channel_add_close();
    channel_refresh_list();
}

static void channel_add_cancel_cb(lv_event_t* e) {
    (void)e;
    channel_add_close();
}

static void channel_add_open_cb(lv_event_t* e) {
    (void)e;
    if (s_ch_add_overlay)
        return;

    ui_focus_push_modal();

    lv_obj_t* scr = lv_screen_active();
    s_ch_add_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_ch_add_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ch_add_overlay, BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_ch_add_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_ch_add_overlay, 0, 0);

    lv_obj_t* panel = lv_obj_create(s_ch_add_overlay);
    lv_obj_set_size(panel, 290, 170);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, BR_RADIUS, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 4, 0);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Add Channel");
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    s_ch_name_ta = lv_textarea_create(panel);
    lv_obj_set_size(s_ch_name_ta, 270, 36);
    lv_textarea_set_max_length(s_ch_name_ta, 19);
    lv_textarea_set_one_line(s_ch_name_ta, true);
    lv_textarea_set_placeholder_text(s_ch_name_ta, "Channel name");
    lv_obj_set_style_bg_color(s_ch_name_ta, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_text_font(s_ch_name_ta, &lv_font_montserrat_12, 0);

    s_ch_psk_ta = lv_textarea_create(panel);
    lv_obj_set_size(s_ch_psk_ta, 270, 36);
    lv_textarea_set_max_length(s_ch_psk_ta, 31);
    lv_textarea_set_one_line(s_ch_psk_ta, true);
    lv_textarea_set_placeholder_text(s_ch_psk_ta, "PSK (optional)");
    lv_obj_set_style_bg_color(s_ch_psk_ta, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_text_font(s_ch_psk_ta, &lv_font_montserrat_12, 0);

    lv_obj_t* btn_row = lv_obj_create(panel);
    lv_obj_set_size(btn_row, 270, 36);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(cancel_btn, 120, 30);
    lv_obj_align(cancel_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, BR_COLOR_SURFACE_2, 0);
    lv_obj_add_event_cb(cancel_btn, channel_add_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);

    lv_obj_t* save_btn = lv_btn_create(btn_row);
    lv_obj_set_size(save_btn, 120, 30);
    lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(save_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_add_event_cb(save_btn, channel_add_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Add");
    lv_obj_center(save_lbl);

    lv_group_t* g = ui_focus_active_group();
    if (g) {
        lv_group_add_obj(g, s_ch_name_ta);
        lv_group_add_obj(g, s_ch_psk_ta);
        lv_group_add_obj(g, cancel_btn);
        lv_group_add_obj(g, save_btn);
        lv_group_focus_obj(s_ch_name_ta);
    }
}

static void do_remove_channel(void* user_data) {
    int index = (int)(intptr_t)user_data;
    int rc = mesh_remove_channel(index);
    if (rc == 0) {
        ESP_LOGI(TAG, "Channel %d removed", index);
        ui_toast_show("Channel removed");
    } else {
        ESP_LOGE(TAG, "Failed to remove channel %d", index);
        ui_toast_show("Remove failed");
    }
    channel_refresh_list();
}

static void channel_remove_cb(lv_event_t* e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    ui_confirm_show("Remove this channel?", "Remove", do_remove_channel, (void*)(intptr_t)index);
}

static void channel_refresh_async(void* arg) {
    (void)arg;
    channel_refresh_list();
}

static void channel_set_default_cb(lv_event_t* e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    mesh_set_default_channel(index);
    ESP_LOGI(TAG, "Default channel set to %d", index);
    ui_toast_show("Default channel set");
    /* The rebuild cleans the channel section that owns this very button, so it
     * cannot run inline (see ui_defer). */
    ui_defer(channel_refresh_async, NULL);
}

static void channel_refresh_list(void) {
    if (!s_channel_section_cont)
        return;
    /* Delete all children of the channel section and rebuild */
    lv_obj_clean(s_channel_section_cont);
    build_channel_manager_section(s_channel_section_cont, s_channel_group);
}

static void build_channel_manager_section(lv_obj_t* cont, lv_group_t* g) {
    ui_zone_track(&s_channel_section_cont, cont);
    s_channel_group = g;

    lv_obj_t* section_lbl = lv_label_create(cont);
    lv_label_set_text(section_lbl, LV_SYMBOL_LIST " Channels");
    lv_obj_set_style_text_font(section_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(section_lbl, BR_COLOR_TEXT, 0);

    int num_channels = mesh_get_channel_count();
    int default_idx = -1;
    mesh_get_channel_info(&default_idx);

    if (num_channels == 0) {
        lv_obj_t* empty = lv_label_create(cont);
        lv_label_set_text(empty, "(no channels)");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(empty, BR_COLOR_TEXT_SEC, 0);
    }

    for (int i = 0; i < num_channels; i++) {
        const char* name = mesh_get_channel_name(i);
        bool has_psk = false;
        uint16_t epoch = 0;
        mesh_get_channel_security(i, &has_psk, &epoch);
        bool is_default = (i == default_idx);

        lv_obj_t* row = lv_obj_create(cont);
        lv_obj_set_size(row, 304, BR_TAP_TARGET_MIN);
        lv_obj_set_style_bg_color(row, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, BR_RADIUS, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* Channel index + name */
        lv_obj_t* lbl = lv_label_create(row);
        char ch_text[48];
        snprintf(ch_text, sizeof(ch_text), "#%d %s%s%s", i, name ? name : "?",
                 has_psk ? " " LV_SYMBOL_EYE_CLOSE : "", is_default ? " *" : "");
        lv_label_set_text(lbl, ch_text);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, is_default ? BR_COLOR_PRIMARY : BR_COLOR_TEXT, 0);
        lv_obj_set_width(lbl, 160);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        /* Set default button (if not already default) */
        if (!is_default) {
            lv_obj_t* def_btn = lv_btn_create(row);
            lv_obj_set_size(def_btn, 50, 24);
            lv_obj_align(def_btn, LV_ALIGN_RIGHT_MID, -40, 0);
            lv_obj_set_style_bg_color(def_btn, BR_COLOR_SURFACE_2, 0);
            lv_obj_set_style_radius(def_btn, 4, 0);
            lv_obj_t* def_lbl = lv_label_create(def_btn);
            lv_label_set_text(def_lbl, "Def");
            lv_obj_set_style_text_font(def_lbl, &lv_font_montserrat_12, 0);
            lv_obj_center(def_lbl);
            lv_obj_add_event_cb(def_btn, channel_set_default_cb, LV_EVENT_CLICKED,
                                (void*)(intptr_t)i);
            if (g)
                lv_group_add_obj(g, def_btn);
        }

        /* Remove button */
        if (!is_default) {
            lv_obj_t* rm_btn = lv_btn_create(row);
            lv_obj_set_size(rm_btn, 30, 24);
            lv_obj_align(rm_btn, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_bg_color(rm_btn, BR_COLOR_DANGER, 0);
            lv_obj_set_style_radius(rm_btn, 4, 0);
            lv_obj_t* rm_lbl = lv_label_create(rm_btn);
            lv_label_set_text(rm_lbl, LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_font(rm_lbl, &lv_font_montserrat_12, 0);
            lv_obj_center(rm_lbl);
            lv_obj_add_event_cb(rm_btn, channel_remove_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
            if (g)
                lv_group_add_obj(g, rm_btn);
        }
    }

    /* Add channel button */
    lv_obj_t* add_btn = lv_btn_create(cont);
    lv_obj_set_size(add_btn, 304, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(add_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(add_btn, BR_RADIUS, 0);
    lv_obj_t* add_lbl = lv_label_create(add_btn);
    lv_label_set_text(add_lbl, LV_SYMBOL_PLUS " Add Channel");
    lv_obj_set_style_text_font(add_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(add_lbl);
    lv_obj_add_event_cb(add_btn, channel_add_open_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, add_btn);
}

/* ── Subpage ─────────────────────────────────────────────────────────────── */

void settings_channels_summary(char* buf, size_t n) {
    int count = mesh_get_channel_count();
    snprintf(buf, n, "%d channel%s", count, count == 1 ? "" : "s");
}

void settings_channels_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    lv_obj_t* cont = settings_subpage_begin(layout, "Channels");
    lv_group_t* g = lv_group_get_default();

    /* Container for channel list (cleaned + rebuilt on add/remove) */
    lv_obj_t* ch_cont = lv_obj_create(cont);
    lv_obj_set_width(ch_cont, 308);
    lv_obj_set_height(ch_cont, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ch_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch_cont, 0, 0);
    lv_obj_set_style_pad_all(ch_cont, 0, 0);
    lv_obj_set_flex_flow(ch_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ch_cont, 4, 0);
    build_channel_manager_section(ch_cont, g);
}
