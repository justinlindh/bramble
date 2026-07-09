#include "scr_channel_create.h"
#include "scr_chat_list.h"
#include "theme/bramble_theme.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "scr_ch_create";

extern void scr_chat_messages_open(bramble_layout_t* layout, int channel_idx);
extern int mesh_add_channel(const char* name, const uint8_t* psk, size_t psk_len);

static lv_obj_t* s_name_ta = NULL;
static lv_obj_t* s_psk_ta = NULL;
static lv_obj_t* s_error_lbl = NULL;
static bramble_layout_t* s_layout = NULL;

static void cancel_click_cb(lv_event_t* e) {
    bramble_layout_t* layout = (bramble_layout_t*)lv_event_get_user_data(e);
    /* Show tab bar */
    layout_set_tab_bar_hidden(layout, false);
    /* Return to chat list */
    scr_chat_list_refresh(layout);
    scr_chat_list_create(layout);
}

static void create_click_cb(lv_event_t* e) {
    (void)e;

    if (!s_name_ta) {
        ESP_LOGE(TAG, "Name textarea is NULL");
        return;
    }

    const char* name = lv_textarea_get_text(s_name_ta);
    const char* psk = s_psk_ta ? lv_textarea_get_text(s_psk_ta) : NULL;

    /* Validate name */
    if (!name || strlen(name) == 0) {
        if (s_error_lbl) {
            lv_label_set_text(s_error_lbl, "Channel name required");
            lv_obj_clear_flag(s_error_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* Create channel */
    size_t psk_len = (psk && strlen(psk) > 0) ? strlen(psk) : 0;
    const uint8_t* psk_bytes = psk_len > 0 ? (const uint8_t*)psk : NULL;

    int idx = mesh_add_channel(name, psk_bytes, psk_len);

    if (idx < 0) {
        if (s_error_lbl) {
            lv_label_set_text(s_error_lbl, "Failed to create channel");
            lv_obj_clear_flag(s_error_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        ESP_LOGE(TAG, "mesh_add_channel failed for '%s'", name);
        return;
    }

    ESP_LOGI(TAG, "Created channel '%s' at index %d", name, idx);

    /* Success: open the new channel's message view */
    scr_chat_messages_open(s_layout, idx);
}

void scr_channel_create_open(bramble_layout_t* layout) {
    s_layout = layout;

    /* Hide tab bar */
    layout_set_tab_bar_hidden(layout, true);

    /* Expand content area */
    lv_obj_clean(layout->content_area);
    lv_obj_set_size(layout->content_area, 320, 240 - BR_STATUS_BAR_H);

    int content_h = 240 - BR_STATUS_BAR_H;

    /* Header */
    lv_obj_t* header = lv_obj_create(layout->content_area);
    lv_obj_set_size(header, 320, 28);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "New Channel");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Form container */
    lv_obj_t* form = lv_obj_create(layout->content_area);
    lv_obj_set_size(form, 320, content_h - 28);
    lv_obj_set_pos(form, 0, 28);
    lv_obj_set_style_bg_opa(form, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(form, 0, 0);
    lv_obj_set_style_pad_all(form, 12, 0);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(form, 8, 0);
    lv_obj_set_scroll_dir(form, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(form, LV_SCROLLBAR_MODE_OFF);

    /* Name label */
    lv_obj_t* name_lbl = lv_label_create(form);
    lv_label_set_text(name_lbl, "Channel name:");
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(name_lbl, BR_COLOR_TEXT, 0);

    /* Name textarea */
    s_name_ta = lv_textarea_create(form);
    lv_obj_set_width(s_name_ta, LV_PCT(100));
    lv_obj_set_height(s_name_ta, 40);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_max_length(s_name_ta, 31);
    lv_textarea_set_placeholder_text(s_name_ta, "Enter channel name");
    lv_obj_set_style_bg_color(s_name_ta, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_border_color(s_name_ta, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_name_ta, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(s_name_ta, &lv_font_montserrat_12, 0);

    lv_group_t* g = lv_group_get_default();
    if (g)
        lv_group_add_obj(g, s_name_ta);

    /* PSK label */
    lv_obj_t* psk_lbl = lv_label_create(form);
    lv_label_set_text(psk_lbl, "Passphrase (optional):");
    lv_obj_set_style_text_font(psk_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(psk_lbl, BR_COLOR_TEXT_SEC, 0);

    /* PSK textarea */
    s_psk_ta = lv_textarea_create(form);
    lv_obj_set_width(s_psk_ta, LV_PCT(100));
    lv_obj_set_height(s_psk_ta, 40);
    lv_textarea_set_one_line(s_psk_ta, true);
    lv_textarea_set_max_length(s_psk_ta, 31);
    lv_textarea_set_placeholder_text(s_psk_ta, "Optional passphrase");
    lv_obj_set_style_bg_color(s_psk_ta, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_border_color(s_psk_ta, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_psk_ta, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(s_psk_ta, &lv_font_montserrat_12, 0);
    if (g)
        lv_group_add_obj(g, s_psk_ta);

    /* Error label (hidden by default) */
    s_error_lbl = lv_label_create(form);
    lv_label_set_text(s_error_lbl, "");
    lv_obj_set_style_text_color(s_error_lbl, lv_color_hex(0xFF5555), 0);
    lv_obj_set_style_text_font(s_error_lbl, &lv_font_montserrat_12, 0);
    lv_obj_add_flag(s_error_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Button container */
    lv_obj_t* btn_cont = lv_obj_create(form);
    lv_obj_set_width(btn_cont, LV_PCT(100));
    lv_obj_set_height(btn_cont, 40);
    lv_obj_set_style_bg_opa(btn_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_cont, 0, 0);
    lv_obj_set_style_pad_all(btn_cont, 0, 0);
    lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* Cancel button */
    lv_obj_t* cancel_btn = lv_btn_create(btn_cont);
    lv_obj_set_size(cancel_btn, 120, 36);
    lv_obj_set_style_bg_color(cancel_btn, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_radius(cancel_btn, BR_RADIUS, 0);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, cancel_click_cb, LV_EVENT_CLICKED, layout);
    if (g)
        lv_group_add_obj(g, cancel_btn);

    /* Create button */
    lv_obj_t* create_btn = lv_btn_create(btn_cont);
    lv_obj_set_size(create_btn, 120, 36);
    lv_obj_set_style_bg_color(create_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(create_btn, BR_RADIUS, 0);
    lv_obj_t* create_lbl = lv_label_create(create_btn);
    lv_label_set_text(create_lbl, "Create");
    lv_obj_set_style_text_font(create_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(create_lbl);
    lv_obj_add_event_cb(create_btn, create_click_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, create_btn);
}
