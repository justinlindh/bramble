#include "scr_chat_list.h"
#include "theme/bramble_theme.h"
#include "msg_store.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "scr_chat";

/* Forward declare — message view from Task 9 */
extern void scr_chat_messages_open(bramble_layout_t *layout, int channel_idx);

static void compose_click_cb(lv_event_t *e) {
    bramble_layout_t *layout = (bramble_layout_t *)lv_event_get_user_data(e);
    scr_chat_messages_open(layout, 0);
}

static void msg_item_click_cb(lv_event_t *e) {
    bramble_layout_t *layout = (bramble_layout_t *)lv_event_get_user_data(e);
    scr_chat_messages_open(layout, 0);
}

void scr_chat_list_create(bramble_layout_t *layout) {
    lv_obj_t *cont = layout_get_content(layout);
    
    /* Header row with title and compose button */
    lv_obj_t *header = lv_obj_create(cont);
    lv_obj_set_size(header, 320, 28);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Messages");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 4, 0);
    
    /* Compose button */
    lv_obj_t *compose_btn = lv_btn_create(header);
    lv_obj_set_size(compose_btn, 80, 24);
    lv_obj_align(compose_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(compose_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(compose_btn, BR_RADIUS, 0);
    lv_obj_t *compose_lbl = lv_label_create(compose_btn);
    lv_label_set_text(compose_lbl, LV_SYMBOL_PLUS " New");
    lv_obj_set_style_text_font(compose_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(compose_lbl);
    lv_obj_add_event_cb(compose_btn, compose_click_cb, LV_EVENT_CLICKED, layout);
    
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_add_obj(g, compose_btn);
    
    /* Scrollable message list */
    lv_obj_t *list = lv_obj_create(cont);
    lv_obj_set_size(list, 320, BR_CONTENT_H - 28);
    lv_obj_set_pos(list, 0, 28);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);
    
    int count = msg_store_count();
    
    if (count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "No messages yet.\nTap 'New' to compose.");
        lv_obj_set_style_text_color(empty, BR_COLOR_TEXT_SEC, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(empty);
        return;
    }
    
    /* Show messages newest-first (store is oldest-first) */
    int start = count > 10 ? count - 10 : 0;  /* Show last 10 */
    for (int i = count - 1; i >= start; i--) {
        const stored_msg_t *msg = msg_store_get(i);
        if (!msg) continue;
        
        bool is_outgoing = (msg->direction == MSG_DIR_OUTGOING || 
                           msg->direction == MSG_DIR_BROADCAST_OUT);
        
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_set_size(card, 304, 48);
        lv_obj_set_style_bg_color(card, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, BR_RADIUS, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        
        /* Focus style for trackball */
        lv_obj_set_style_bg_color(card, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(card, LV_OPA_30, LV_STATE_FOCUSED);
        if (g) lv_group_add_obj(g, card);
        
        /* Direction indicator + peer address */
        char header_buf[32];
        if (is_outgoing) {
            snprintf(header_buf, sizeof(header_buf), LV_SYMBOL_RIGHT " You");
        } else {
            snprintf(header_buf, sizeof(header_buf), LV_SYMBOL_LEFT " %08lX",
                     (unsigned long)msg->peer_addr);
        }
        lv_obj_t *hdr = lv_label_create(card);
        lv_label_set_text(hdr, header_buf);
        lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hdr, is_outgoing ? BR_COLOR_SENT : BR_COLOR_PRIMARY, 0);
        lv_obj_set_pos(hdr, 0, 0);
        
        /* Message preview */
        lv_obj_t *preview = lv_label_create(card);
        lv_label_set_text(preview, msg->text);
        lv_obj_set_style_text_font(preview, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(preview, BR_COLOR_TEXT, 0);
        lv_obj_set_pos(preview, 0, 16);
        lv_label_set_long_mode(preview, LV_LABEL_LONG_DOT);
        lv_obj_set_width(preview, 290);
        
        lv_obj_add_event_cb(card, msg_item_click_cb, LV_EVENT_CLICKED, layout);
    }
}

void scr_chat_list_refresh(bramble_layout_t *layout) {
    layout_set_tab(layout, TAB_CHAT);
}
