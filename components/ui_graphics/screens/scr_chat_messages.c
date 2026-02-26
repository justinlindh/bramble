#include "scr_chat_messages.h"
#include "scr_chat_list.h"
#include "theme/bramble_theme.h"
#include "msg_store.h"
#include "chat_target.h"
#include "chat_unread.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "scr_msg";
static int s_active_channel = -1;
static chat_target_t s_target;
static lv_obj_t *s_msg_list = NULL;
static lv_obj_t *s_compose_ta = NULL;
static lv_obj_t *s_title = NULL;
static uint32_t s_selected_packet_id = 0;

static void render_messages_for_target(void);

/* Use extern for mesh_send — it's in main, not a component */
extern int mesh_send_broadcast(const uint8_t *data, size_t len);
extern uint32_t mesh_send_channel(int channel_idx, uint32_t dest_addr, const uint8_t *data, size_t len);
extern uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t *data, size_t len);
extern int mesh_get_channel_count(void);
extern const char *mesh_get_channel_name(int index);
extern const char *mesh_get_peer_name(uint32_t addr);

static void update_title(void) {
    if (!s_title) return;
    if (s_target.kind == CHAT_TARGET_BROADCAST) {
        lv_label_set_text(s_title, "Broadcast");
    } else if (s_target.kind == CHAT_TARGET_CHANNEL) {
        const char *name = mesh_get_channel_name((int)s_target.channel_index);
        if (name && name[0]) {
            lv_label_set_text(s_title, name);
        } else {
            static char buf[24];
            snprintf(buf, sizeof(buf), "Channel %d", (int)s_target.channel_index);
            lv_label_set_text(s_title, buf);
        }
    } else {
        const char *peer_name = mesh_get_peer_name(s_target.peer_addr);
        static char buf[32];
        if (peer_name && peer_name[0]) {
            snprintf(buf, sizeof(buf), "DM: %s", peer_name);
        } else {
            snprintf(buf, sizeof(buf), "DM: %08lX", (unsigned long)s_target.peer_addr);
        }
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
    s_selected_packet_id = 0;
    scr_chat_list_create(layout);
}

static void send_current_message(void) {
    if (!s_compose_ta) return;
    const char *text = lv_textarea_get_text(s_compose_ta);
    if (!text || !text[0]) return;

    size_t len = strlen(text);
    int rc = 0;
    ESP_LOGI(TAG, "send_click target kind=%d ch=%d active=%d len=%u", (int)s_target.kind, (int)s_target.channel_index, s_active_channel, (unsigned)len);
    if (s_target.kind == CHAT_TARGET_BROADCAST) {
        rc = mesh_send_broadcast((const uint8_t *)text, len);
    } else if (s_target.kind == CHAT_TARGET_CHANNEL) {
        rc = (mesh_send_channel((int)s_target.channel_index, 0xFFFFFFFF, (const uint8_t *)text, len) != 0) ? 0 : -1;
    } else {
        rc = (mesh_send_message(s_target.peer_addr, (const uint8_t *)text, len) != 0) ? 0 : -1;
    }

    if (rc == 0) {
        lv_textarea_set_text(s_compose_ta, "");
        render_messages_for_target();
    } else {
        ESP_LOGW(TAG, "send failed for target kind=%d ch=%d", (int)s_target.kind, (int)s_target.channel_index);
    }
}

static void send_click_cb(lv_event_t *e) {
    (void)e;
    send_current_message();
}

static void compose_ready_cb(lv_event_t *e) {
    (void)e;
    send_current_message();
}

static void msg_bubble_click_cb(lv_event_t *e) {
    uint32_t packet_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (packet_id == 0) {
        return;
    }

    if (s_selected_packet_id == packet_id) {
        s_selected_packet_id = 0;
    } else {
        s_selected_packet_id = packet_id;
    }
    render_messages_for_target();
}

static void format_route_text(char *out,
                              size_t out_len,
                              uint8_t hop_count,
                              const uint32_t *hops) {
    if (!out || out_len == 0) return;
    if (!hops || hop_count == 0) {
        snprintf(out, out_len, "Route unavailable");
        return;
    }

    size_t pos = 0;
    for (uint8_t i = 0; i < hop_count; i++) {
        const char *peer_name = mesh_get_peer_name(hops[i]);
        char node_buf[16];
        if (peer_name && peer_name[0]) {
            snprintf(node_buf, sizeof(node_buf), "%s", peer_name);
        } else {
            snprintf(node_buf, sizeof(node_buf), "%08lX", (unsigned long)hops[i]);
        }

        int n = 0;
        if (i == 0) {
            n = snprintf(out + pos, out_len - pos, "%s", node_buf);
        } else {
            n = snprintf(out + pos, out_len - pos, " -> %s", node_buf);
        }
        if (n < 0 || (size_t)n >= (out_len - pos)) {
            out[out_len - 1] = '\0';
            return;
        }
        pos += (size_t)n;
    }
}

static void add_message_bubble(lv_obj_t *parent, const char *sender,
                                const stored_msg_t *msg, bool is_mine) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
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
    lv_label_set_text(msg_lbl, msg->text);
    lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg_lbl, BR_COLOR_TEXT, 0);
    lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg_lbl, LV_PCT(100));

    /* Delivery status badge — only on outgoing messages */
    if (is_mine) {
        const char *badge_sym;
        lv_color_t  badge_color;

        switch (msg->status) {
            case MSG_STATUS_SENT:
                /* Single check — transmitted, awaiting ACK */
                badge_sym   = LV_SYMBOL_OK;
                badge_color = BR_COLOR_TEXT_SEC;   /* muted gray */
                break;
            case MSG_STATUS_DELIVERED:
                /* Double check — ACK received */
                badge_sym   = LV_SYMBOL_OK " " LV_SYMBOL_OK;
                badge_color = BR_COLOR_PRIMARY;
                break;
            case MSG_STATUS_FAILED:
                /* X — max retries exhausted */
                badge_sym   = LV_SYMBOL_CLOSE;
                badge_color = BR_COLOR_DANGER;     /* red */
                break;
            default:
                /* Bullet — queued / no ACK tracking (e.g. broadcast) */
                badge_sym   = LV_SYMBOL_BULLET;
                badge_color = BR_COLOR_TEXT_SEC;   /* muted gray */
                break;
        }

        lv_obj_t *status_lbl = lv_label_create(bubble);
        lv_label_set_text(status_lbl, badge_sym);
        lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(status_lbl, badge_color, 0);
        lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(status_lbl, LV_PCT(100));
    }

    bool can_show_route = is_mine && msg->status == MSG_STATUS_DELIVERED && msg->route_hop_count > 1;
    if (can_show_route) {
        bool expanded = (s_selected_packet_id != 0 && msg->packet_id == s_selected_packet_id);

        lv_obj_t *hint_lbl = lv_label_create(bubble);
        lv_label_set_text(hint_lbl, expanded ? "Hide route" : "Show route");
        lv_obj_set_style_text_font(hint_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint_lbl, BR_COLOR_TEXT_SEC, 0);

        if (expanded) {
            char route_buf[200];
            format_route_text(route_buf, sizeof(route_buf), msg->route_hop_count, msg->route_hops);

            lv_obj_t *route_box = lv_obj_create(bubble);
            lv_obj_set_width(route_box, LV_PCT(100));
            lv_obj_set_height(route_box, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(route_box, BR_COLOR_SURFACE_2, 0);
            lv_obj_set_style_bg_opa(route_box, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(route_box, 0, 0);
            lv_obj_set_style_pad_all(route_box, 4, 0);
            lv_obj_clear_flag(route_box, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *route_lbl = lv_label_create(route_box);
            lv_label_set_text(route_lbl, route_buf);
            lv_label_set_long_mode(route_lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(route_lbl, LV_PCT(100));
            lv_obj_set_style_text_font(route_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(route_lbl, BR_COLOR_TEXT, 0);
        }

        if (msg->packet_id != 0) {
            lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(bubble, msg_bubble_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)msg->packet_id);
        }
    }
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

        char sender[20];
        if (!is_mine) {
            /* Try to get peer name first, fallback to hex address */
            const char *peer_name = mesh_get_peer_name(msg->peer_addr);
            if (peer_name) {
                snprintf(sender, sizeof(sender), "%s", peer_name);
            } else {
                snprintf(sender, sizeof(sender), "%08lX", (unsigned long)msg->peer_addr);
            }
        }

        add_message_bubble(s_msg_list, is_mine ? NULL : sender, msg, is_mine);
    }

    lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void open_with_target(bramble_layout_t *layout, chat_target_t target, int clear_channel_idx) {
    s_target = target;
    s_active_channel = (s_target.kind == CHAT_TARGET_CHANNEL) ? s_target.channel_index : 0;
    s_selected_packet_id = 0;

    if (clear_channel_idx >= 0) {
        chat_unread_clear_for_channel(clear_channel_idx);
    }

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
    if (s_target.kind == CHAT_TARGET_DM) {
        lv_obj_add_flag(target_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (g) {
        lv_group_add_obj(g, target_btn);
    }

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
    lv_obj_set_scrollbar_mode(s_msg_list, LV_SCROLLBAR_MODE_OFF);  /* Hide stray bars */

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
    lv_obj_add_event_cb(s_compose_ta, compose_ready_cb, LV_EVENT_READY, NULL);  /* Enter key sends */
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


void scr_chat_messages_open(bramble_layout_t *layout, int channel_idx) {
    chat_target_t target = (channel_idx > 0)
        ? chat_target_normalize(CHAT_TARGET_CHANNEL, channel_idx, mesh_get_channel_count())
        : chat_target_default();
    open_with_target(layout, target, channel_idx);
}

void scr_chat_messages_open_dm(bramble_layout_t *layout, uint32_t peer_addr) {
    open_with_target(layout, chat_target_dm(peer_addr), -1);
}

void scr_chat_messages_on_recv(void) {
    render_messages_for_target();
}
