#include "scr_chat_messages.h"
#include "scr_chat_list.h"
#include "theme/bramble_theme.h"
#include "msg_store.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "scr_msg";
static int s_active_channel = -1;
static lv_obj_t *s_msg_list = NULL;
static lv_obj_t *s_compose_ta = NULL;

/* Use extern for mesh_send — it's in main, not a component */
extern int mesh_send_broadcast(const uint8_t *data, size_t len);

static void back_click_cb(lv_event_t *e) {
    bramble_layout_t *layout = (bramble_layout_t *)lv_event_get_user_data(e);
    /* Restore tab bar */
    lv_obj_clear_flag(layout->tab_bar, LV_OBJ_FLAG_HIDDEN);
    /* Restore content area size */
    lv_obj_set_size(layout->content_area, 320, BR_CONTENT_H);
    lv_obj_set_pos(layout->content_area, 0, BR_STATUS_BAR_H);
    /* Rebuild chat list */
    lv_obj_clean(layout->content_area);
    s_active_channel = -1;
    s_msg_list = NULL;
    s_compose_ta = NULL;
    scr_chat_list_create(layout);
}

static void send_click_cb(lv_event_t *e) {
    (void)e;
    if (!s_compose_ta) return;
    const char *text = lv_textarea_get_text(s_compose_ta);
    if (!text || !text[0]) return;
    
    size_t len = strlen(text);
    mesh_send_broadcast((const uint8_t *)text, len);
    lv_textarea_set_text(s_compose_ta, "");
    
    /* TODO: refresh message list to show sent message */
}

static void add_message_bubble(lv_obj_t *parent, const char *sender,
                                const char *text, bool is_mine) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 304, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bubble, 220, 0);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, is_mine ? BR_COLOR_SENT : BR_COLOR_RECV, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 6, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);

    if (is_mine) {
        lv_obj_align(bubble, LV_ALIGN_RIGHT_MID, 0, 0);
    } else {
        lv_obj_align(bubble, LV_ALIGN_LEFT_MID, 0, 0);
    }

    /* Sender name (only for received) */
    if (!is_mine && sender) {
        lv_obj_t *name_lbl = lv_label_create(bubble);
        lv_label_set_text(name_lbl, sender);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(name_lbl, BR_COLOR_PRIMARY, 0);
    }

    lv_obj_t *msg_lbl = lv_label_create(bubble);
    lv_label_set_text(msg_lbl, text);
    lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg_lbl, BR_COLOR_TEXT, 0);
    lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg_lbl, LV_PCT(100));
}

void scr_chat_messages_open(bramble_layout_t *layout, int channel_idx) {
    s_active_channel = channel_idx;
    
    /* Hide tab bar */
    lv_obj_add_flag(layout->tab_bar, LV_OBJ_FLAG_HIDDEN);
    
    /* Expand content area to fill space left by hidden tab bar */
    lv_obj_clean(layout->content_area);
    lv_obj_set_size(layout->content_area, 320, 240 - BR_STATUS_BAR_H);
    
    int content_h = 240 - BR_STATUS_BAR_H;
    
    /* Header with back button + title */
    lv_obj_t *header = lv_obj_create(layout->content_area);
    lv_obj_set_size(header, 320, 28);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 40, 24);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, back_click_cb, LV_EVENT_CLICKED, layout);
    
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_add_obj(g, back_btn);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Chat");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    
    /* Message list area */
    int msg_area_h = content_h - 28 - BR_COMPOSE_BAR_H;
    s_msg_list = lv_obj_create(layout->content_area);
    lv_obj_set_size(s_msg_list, 320, msg_area_h);
    lv_obj_set_pos(s_msg_list, 0, 28);
    lv_obj_set_style_bg_opa(s_msg_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_msg_list, 0, 0);
    lv_obj_set_style_pad_all(s_msg_list, 4, 0);
    lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_msg_list, 4, 0);

    /* Load messages from store */
    int count = msg_store_count();
    
    for (int i = 0; i < count; i++) {
        const stored_msg_t *msg = msg_store_get(i);
        if (!msg) continue;
        
        bool is_mine = (msg->direction == MSG_DIR_OUTGOING ||
                       msg->direction == MSG_DIR_BROADCAST_OUT);
        
        char sender[16];
        if (!is_mine) {
            snprintf(sender, sizeof(sender), "%08lX", (unsigned long)msg->peer_addr);
        }
        
        add_message_bubble(s_msg_list, is_mine ? NULL : sender, msg->text, is_mine);
    }

    /* Scroll to bottom */
    lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_OFF);
    
    /* Compose bar */
    lv_obj_t *compose_bar = lv_obj_create(layout->content_area);
    lv_obj_set_size(compose_bar, 320, BR_COMPOSE_BAR_H);
    lv_obj_set_pos(compose_bar, 0, content_h - BR_COMPOSE_BAR_H);
    lv_obj_set_style_bg_color(compose_bar, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(compose_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(compose_bar, 0, 0);
    lv_obj_set_style_border_width(compose_bar, 0, 0);
    lv_obj_set_style_pad_all(compose_bar, 4, 0);
    lv_obj_clear_flag(compose_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_compose_ta = lv_textarea_create(compose_bar);
    lv_obj_set_size(s_compose_ta, 260, 36);
    lv_obj_set_pos(s_compose_ta, 0, 0);
    lv_textarea_set_placeholder_text(s_compose_ta, "Type message...");
    lv_textarea_set_one_line(s_compose_ta, true);
    lv_obj_set_style_bg_color(s_compose_ta, BR_COLOR_BG, 0);
    lv_obj_set_style_text_color(s_compose_ta, BR_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_compose_ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(s_compose_ta, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
    if (g) lv_group_add_obj(g, s_compose_ta);

    lv_obj_t *send_btn = lv_btn_create(compose_bar);
    lv_obj_set_size(send_btn, 44, 36);
    lv_obj_set_pos(send_btn, 264, 0);
    lv_obj_set_style_bg_color(send_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_t *send_lbl = lv_label_create(send_btn);
    lv_label_set_text(send_lbl, LV_SYMBOL_OK);
    lv_obj_center(send_lbl);
    lv_obj_add_event_cb(send_btn, send_click_cb, LV_EVENT_CLICKED, NULL);
    if (g) lv_group_add_obj(g, send_btn);
}

void scr_chat_messages_on_recv(void) {
    /* TODO: if message view is open, add new bubble */
}
