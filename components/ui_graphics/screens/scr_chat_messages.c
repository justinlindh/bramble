#include "scr_chat_messages.h"
#include "scr_chat_list.h"
#include "theme/bramble_theme.h"
#include "msg_store.h"
#include "chat_target.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "scr_msg";
static int s_active_channel = -1;
static chat_target_t s_target;
static lv_obj_t *s_msg_list = NULL;
static lv_obj_t *s_compose_ta = NULL;
static lv_obj_t *s_title = NULL;

static void render_messages_for_target(void);

/* Use extern for mesh_send — it's in main, not a component */
extern int mesh_send_broadcast(const uint8_t *data, size_t len);
extern uint32_t mesh_send_channel(int channel_idx, uint32_t dest_addr, const uint8_t *data, size_t len);
extern int mesh_get_channel_count(void);

static void update_title(void) {
    if (!s_title) return;
    if (s_target.kind == CHAT_TARGET_BROADCAST) {
        lv_label_set_text(s_title, "Broadcast");
    } else {
        static char buf[24];
        snprintf(buf, sizeof(buf), "Channel %d", (int)s_target.channel_index);
        lv_label_set_text(s_title, buf);
    }
}

static bool message_matches_target(const stored_msg_t *msg) {
    int msg_channel = msg ? (int)msg->channel_index : -1;
    return chat_target_matches_message(s_target, msg, msg_channel);
}

static void channel_cycle_click_cb(lv_event_t *e) {
    (void)e;
    s_target = chat_target_cycle(s_target, mesh_get_channel_count());
    s_active_channel = (s_target.kind == CHAT_TARGET_CHANNEL) ? s_target.channel_index : 0;
    update_title();
    render_messages_for_target();
}

static void back_click_cb(lv_event_t *e) {
    bramble_layout_t *layout = (bramble_layout_t *)lv_event_get_user_data(e);
    /* Restore tab bar */
    lv_obj_clear_flag(layout->tab_bar, LV_OBJ_FLAG_HIDDEN);
    /* Restore content area size */
    lv_obj_set_size(layout->content_area, 320, BR_CONTENT_H);
    lv_obj_set_pos(layout->content_area, 0, BR_STATUS_BAR_H);
    /* Rebuild chat list */
    lv_refr_now(lv_display_get_default());
    lv_obj_clean(layout->content_area);
    s_active_channel = -1;
    s_target = chat_target_default();
    s_msg_list = NULL;
    s_compose_ta = NULL;
    s_title = NULL;
    scr_chat_list_create(layout);
}

static void send_click_cb(lv_event_t *e) {
    (void)e;
    if (!s_compose_ta) return;
    const char *text = lv_textarea_get_text(s_compose_ta);
    if (!text || !text[0]) return;
    
    size_t len = strlen(text);
    int rc = 0;
    ESP_LOGI(TAG, "send_click target kind=%d ch=%d active=%d len=%u", (int)s_target.kind, (int)s_target.channel_index, s_active_channel, (unsigned)len);
    if (s_target.kind == CHAT_TARGET_BROADCAST) {
        rc = mesh_send_broadcast((const uint8_t *)text, len);
    } else {
        rc = (mesh_send_channel((int)s_target.channel_index, 0xFFFFFFFF, (const uint8_t *)text, len) != 0) ? 0 : -1;
    }

    if (rc == 0) {
        lv_textarea_set_text(s_compose_ta, "");
        render_messages_for_target();
    } else {
        ESP_LOGW(TAG, "send failed for target kind=%d ch=%d", (int)s_target.kind, (int)s_target.channel_index);
    }
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
    lv_obj_set_width(bubble, 220);
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

static void render_messages_for_target(void) {
    if (!s_msg_list) return;

    lv_refr_now(lv_display_get_default());
    lv_obj_clean(s_msg_list);

    int count = msg_store_count();
    for (int i = 0; i < count; i++) {
        const stored_msg_t *msg = msg_store_get(i);
        if (!msg || !message_matches_target(msg)) continue;

        bool is_mine = (msg->direction == MSG_DIR_OUTGOING ||
                        msg->direction == MSG_DIR_BROADCAST_OUT);

        char sender[16];
        if (!is_mine) {
            snprintf(sender, sizeof(sender), "%08lX", (unsigned long)msg->peer_addr);
        }

        add_message_bubble(s_msg_list, is_mine ? NULL : sender, msg->text, is_mine);
    }

    lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_OFF);
}

void scr_chat_messages_open(bramble_layout_t *layout, int channel_idx) {
    s_active_channel = channel_idx;
    s_target = (channel_idx > 0)
        ? chat_target_normalize(CHAT_TARGET_CHANNEL, channel_idx, mesh_get_channel_count())
        : chat_target_default();

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

    lv_obj_t *target_btn = lv_btn_create(header);
    lv_obj_set_size(target_btn, 108, 22);
    lv_obj_align(target_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(target_btn, BR_COLOR_SURFACE, 0);
    lv_obj_add_event_cb(target_btn, channel_cycle_click_cb, LV_EVENT_CLICKED, NULL);
    if (g) lv_group_add_obj(g, target_btn);

    s_title = lv_label_create(target_btn);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_title, BR_COLOR_TEXT, 0);
    lv_obj_center(s_title);
    update_title();
    
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
    lv_obj_set_scroll_dir(s_msg_list, LV_DIR_VER);  /* Prevent horizontal scroll */

    /* Load messages from store */
    render_messages_for_target();
    
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
    if (g) {
        lv_group_add_obj(g, s_compose_ta);
        lv_group_focus_obj(s_compose_ta);  /* ensure keyboard types into compose bar immediately */
    }

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
    render_messages_for_target();
}
