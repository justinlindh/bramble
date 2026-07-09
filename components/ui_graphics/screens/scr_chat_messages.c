#include "scr_chat_messages.h"
#include "scr_chat_list.h"
#include "theme/bramble_theme.h"
#include "msg_store.h"
#include "chat_target.h"
#include "chat_unread.h"
#include "chat_message_ui.h"
#include "ui_toast.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static const char* TAG = "scr_msg";
static int s_active_channel = -1;
static chat_target_t s_target;
static lv_obj_t* s_msg_list = NULL;
static lv_obj_t* s_compose_ta = NULL;
static lv_obj_t* s_title = NULL;
static uint32_t s_selected_packet_id = 0;

/* Render bound: newest matching messages built per pass. Keeps LVGL
 * object count sane on deep stores. */
#define CHAT_RENDER_MAX 60

static void render_messages_for_target(bool scroll_to_bottom);

/* Use extern for mesh_send — it's in main, not a component */
extern int mesh_send_broadcast(const uint8_t* data, size_t len);
extern uint32_t mesh_send_channel(int channel_idx, uint32_t dest_addr, const uint8_t* data,
                                  size_t len);
extern uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t* data, size_t len);
extern int mesh_get_channel_count(void);
extern const char* mesh_get_channel_name(int index);
extern const char* mesh_get_peer_name(uint32_t addr);

static void update_title(void) {
    if (!s_title)
        return;

    const char* channel_name = NULL;
    const char* peer_name = NULL;
    if (s_target.kind == CHAT_TARGET_CHANNEL) {
        channel_name = mesh_get_channel_name((int)s_target.channel_index);
    } else if (s_target.kind == CHAT_TARGET_DM) {
        peer_name = mesh_get_peer_name(s_target.peer_addr);
    }

    static char buf[48];
    if (s_target.kind == CHAT_TARGET_DM) {
        if (peer_name && peer_name[0]) {
            snprintf(buf, sizeof(buf), "%s", peer_name);
        } else {
            snprintf(buf, sizeof(buf), "%08lX", (unsigned long)s_target.peer_addr);
        }
    } else if (channel_name) {
        snprintf(buf, sizeof(buf), "#%s", channel_name);
    } else {
        snprintf(buf, sizeof(buf), "Chat");
    }
    lv_label_set_text(s_title, buf);
}

static bool message_matches_target(const stored_msg_t* msg) {
    int msg_channel = msg ? (int)msg->channel_index : -1;
    return chat_target_matches_message(s_target, msg, msg_channel);
}

static void channel_cycle_click_cb(lv_event_t* e) {
    (void)e;
    s_target = chat_target_cycle(s_target, mesh_get_channel_count());
    s_active_channel = (s_target.kind == CHAT_TARGET_CHANNEL) ? s_target.channel_index : 0;
    update_title();
    render_messages_for_target(true);
}

static void back_click_cb(lv_event_t* e) {
    bramble_layout_t* layout = (bramble_layout_t*)lv_event_get_user_data(e);
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
    layout->in_dm_view = false;
    scr_chat_list_create(layout);
}

static void send_current_message(void) {
    if (!s_compose_ta)
        return;
    const char* text = lv_textarea_get_text(s_compose_ta);
    if (!text || !text[0])
        return;

    size_t len = strlen(text);
    int rc = 0;
    ESP_LOGI(TAG, "send_click target kind=%d ch=%d active=%d len=%u", (int)s_target.kind,
             (int)s_target.channel_index, s_active_channel, (unsigned)len);
    if (s_target.kind == CHAT_TARGET_BROADCAST) {
        rc = mesh_send_broadcast((const uint8_t*)text, len);
    } else if (s_target.kind == CHAT_TARGET_CHANNEL) {
        rc = (mesh_send_channel((int)s_target.channel_index, 0xFFFFFFFF, (const uint8_t*)text,
                                len) != 0)
                 ? 0
                 : -1;
    } else {
        rc = (mesh_send_message(s_target.peer_addr, (const uint8_t*)text, len) != 0) ? 0 : -1;
    }

    if (rc == 0) {
        lv_textarea_set_text(s_compose_ta, "");
        render_messages_for_target(true);
    } else {
        ESP_LOGW(TAG, "send failed for target kind=%d ch=%d", (int)s_target.kind,
                 (int)s_target.channel_index);
        ui_toast_show("Send failed");
    }
}

static void send_click_cb(lv_event_t* e) {
    (void)e;
    send_current_message();
}

static void compose_ready_cb(lv_event_t* e) {
    (void)e;
    send_current_message();
}

static void msg_bubble_click_cb(lv_event_t* e) {
    uint32_t packet_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (packet_id == 0) {
        return;
    }

    if (s_selected_packet_id == packet_id) {
        s_selected_packet_id = 0;
    } else {
        s_selected_packet_id = packet_id;
    }
    render_messages_for_target(false);
}

static void format_compact_hop_name(char* out, size_t out_len, uint32_t hop_addr) {
    if (!out || out_len == 0) {
        return;
    }

    const char* peer_name = mesh_get_peer_name(hop_addr);
    if (peer_name && peer_name[0]) {
        /* Compact route UI uses up to 4 chars per hop, no ellipsis. */
        snprintf(out, out_len, "%.4s", peer_name);
    } else {
        snprintf(out, out_len, "%04lX", (unsigned long)(hop_addr & 0xFFFFUL));
    }
}

static bool append_text(char* out, size_t out_len, size_t* pos, const char* text) {
    if (!out || !pos || !text || *pos >= out_len) {
        return false;
    }

    int n = snprintf(out + *pos, out_len - *pos, "%s", text);
    if (n < 0 || (size_t)n >= (out_len - *pos)) {
        out[out_len - 1] = '\0';
        return false;
    }

    *pos += (size_t)n;
    return true;
}

static bool format_route_compact_full(char* out, size_t out_len, uint8_t hop_count,
                                      const uint32_t* hops, size_t* char_len) {
    if (!out || out_len == 0 || !char_len) {
        return false;
    }

    out[0] = '\0';
    *char_len = 0;

    if (!hops || hop_count == 0) {
        return append_text(out, out_len, char_len, "Route unavailable");
    }

    for (uint8_t i = 0; i < hop_count; i++) {
        char hop_buf[8];
        format_compact_hop_name(hop_buf, sizeof(hop_buf), hops[i]);

        if (i > 0 && !append_text(out, out_len, char_len, " → ")) {
            return false;
        }

        if (!append_text(out, out_len, char_len, hop_buf)) {
            return false;
        }
    }

    return true;
}

static bool format_route_compact_endpoints(char* out, size_t out_len, uint8_t hop_count,
                                           const uint32_t* hops) {
    if (!out || out_len == 0) {
        return false;
    }

    out[0] = '\0';
    if (!hops || hop_count == 0) {
        return snprintf(out, out_len, "Route unavailable") > 0;
    }

    if (hop_count == 1) {
        char hop_buf[8];
        format_compact_hop_name(hop_buf, sizeof(hop_buf), hops[0]);
        return snprintf(out, out_len, "%s", hop_buf) > 0;
    }

    char first_buf[8];
    char last_buf[8];
    format_compact_hop_name(first_buf, sizeof(first_buf), hops[0]);
    format_compact_hop_name(last_buf, sizeof(last_buf), hops[hop_count - 1]);
    return snprintf(out, out_len, "%s → … → %s", first_buf, last_buf) > 0;
}

static void format_route_text(char* out, size_t out_len, uint8_t hop_count, const uint32_t* hops) {
    if (!out || out_len == 0) {
        return;
    }

    /* Bubble width is fixed at 220 px; use compact fallback for long routes. */
    const size_t compact_limit_chars = 28;
    size_t full_char_len = 0;
    if (format_route_compact_full(out, out_len, hop_count, hops, &full_char_len) &&
        full_char_len <= compact_limit_chars) {
        return;
    }

    if (!format_route_compact_endpoints(out, out_len, hop_count, hops)) {
        snprintf(out, out_len, "Route unavailable");
    }
}

/* Detect CTCP ACTION: \x01ACTION text\x01 */
static bool msg_is_action(const stored_msg_t* msg) {
    return msg->text_len > 9 && msg->text[0] == '\x01' && strncmp(msg->text + 1, "ACTION ", 7) == 0;
}

static const char* msg_action_text(const stored_msg_t* msg) {
    /* Skip \x01ACTION (8 bytes) */
    return msg->text + 8;
}

static void add_action_line(lv_obj_t* parent, const char* sender, const stored_msg_t* msg,
                            bool is_mine) {
    const char* action = msg_action_text(msg);
    /* Build "* sender action" string */
    static char action_buf[MSG_TEXT_MAX + 32];
    const char* name = is_mine ? "me" : (sender ? sender : "???");
    /* Strip trailing \x01 from display */
    size_t action_len = strlen(action);
    if (action_len > 0 && action[action_len - 1] == '\x01') {
        action_len--;
    }
    snprintf(action_buf, sizeof(action_buf), "* %s %.*s", name, (int)action_len, action);

    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, action_buf);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xDA77F2), 0); /* purple */
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
}

static void add_message_bubble(lv_obj_t* parent, const char* sender, const stored_msg_t* msg,
                               bool is_mine, int age_s) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bubble = lv_obj_create(row);
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
        lv_obj_t* name_lbl = lv_label_create(bubble);
        lv_label_set_text(name_lbl, sender);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(name_lbl, BR_COLOR_PRIMARY, 0);
    }

    lv_obj_t* msg_lbl = lv_label_create(bubble);
    lv_label_set_text(msg_lbl, msg->text);
    lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg_lbl, BR_COLOR_TEXT, 0);
    lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg_lbl, LV_PCT(100));

    bool can_show_route = chat_message_has_inline_route_toggle(
        is_mine, msg->status, msg->route_hop_count, msg->packet_id);

    /* Delivery status badge — only on outgoing messages */
    if (is_mine) {
        chat_delivery_badge_t badge = chat_message_delivery_badge(msg->status);
        const char* badge_sym = LV_SYMBOL_BULLET;
        lv_color_t badge_color = BR_COLOR_TEXT_SEC;

        if (badge.kind == CHAT_DELIVERY_BADGE_SINGLE_CHECK) {
            badge_sym = LV_SYMBOL_OK;
        } else if (badge.kind == CHAT_DELIVERY_BADGE_DOUBLE_CHECK) {
            badge_sym = LV_SYMBOL_OK " " LV_SYMBOL_OK;
        } else if (badge.kind == CHAT_DELIVERY_BADGE_FAILED) {
            badge_sym = LV_SYMBOL_CLOSE;
        }

        if (badge.color_role == CHAT_DELIVERY_COLOR_DELIVERED) {
            badge_color = BR_COLOR_PRIMARY;
        } else if (badge.color_role == CHAT_DELIVERY_COLOR_FAILED) {
            badge_color = BR_COLOR_DANGER;
        }

        static char status_buf[24];
        bool has_route_meta = (msg->route_hop_count > 0);
        if (has_route_meta) {
            if (msg->route_hop_count > 1) {
                snprintf(status_buf, sizeof(status_buf), "%s %u↗", badge_sym,
                         (unsigned)msg->route_hop_count);
            } else {
                /* Single-hop direct route: keep existing badge uncluttered. */
                snprintf(status_buf, sizeof(status_buf), "%s", badge_sym);
            }
            badge_sym = status_buf;
        }

        lv_obj_t* status_lbl = lv_label_create(bubble);
        lv_label_set_text(status_lbl, badge_sym);
        lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(status_lbl, badge_color, 0);
        lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(status_lbl, LV_PCT(100));
    }

    if (can_show_route) {
        bool expanded = (s_selected_packet_id != 0 && msg->packet_id == s_selected_packet_id);

        if (expanded) {
            char route_buf[200];
            format_route_text(route_buf, sizeof(route_buf), msg->route_hop_count, msg->route_hops);

            lv_obj_t* route_lbl = lv_label_create(bubble);
            lv_label_set_text(route_lbl, route_buf);
            lv_label_set_long_mode(route_lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(route_lbl, LV_PCT(100));
            lv_obj_set_style_text_font(route_lbl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(route_lbl, BR_COLOR_TEXT_SEC, 0);
        }

        if (msg->packet_id != 0) {
            lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(bubble, msg_bubble_click_cb, LV_EVENT_CLICKED,
                                (void*)(uintptr_t)msg->packet_id);
        }
    }

    if (age_s < 0)
        return; /* restored from a previous boot: age unknowable, hide it */
    char age_buf[8];
    chat_format_age((uint32_t)age_s, age_buf, sizeof(age_buf));
    lv_obj_t* age_lbl = lv_label_create(bubble);
    lv_label_set_text(age_lbl, age_buf);
    lv_obj_set_style_text_font(age_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(age_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_align(age_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(age_lbl, LV_PCT(100));
}

static void render_messages_for_target(bool scroll_to_bottom) {
    if (!s_msg_list)
        return;

    /* Preserve the reading position unless explicitly asked to jump: a
     * reader scrolled into history must not be yanked by an arrival. */
    int32_t prev_y = lv_obj_get_scroll_y(s_msg_list);
    bool was_at_bottom = (lv_obj_get_scroll_bottom(s_msg_list) <= 8);

    lv_refr_now(lv_display_get_default());
    lv_obj_clean(s_msg_list);

    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    int count = msg_store_count();

    /* Collect the newest CHAT_RENDER_MAX matching messages, then render
     * them oldest-first. Pass 2 re-checks the target: mesh_task can add a
     * message between the passes, shifting ring indices, and a stale index
     * must never leak another conversation's message into this one. */
    int match_idx[CHAT_RENDER_MAX];
    int n_match = 0;
    for (int i = count - 1; i >= 0 && n_match < CHAT_RENDER_MAX; i--) {
        const stored_msg_t* m = msg_store_get(i);
        if (m && message_matches_target(m))
            match_idx[n_match++] = i;
    }

    for (int k = n_match - 1; k >= 0; k--) {
        const stored_msg_t* msg = msg_store_get(match_idx[k]);
        if (!msg || !message_matches_target(msg))
            continue;

        bool is_mine =
            (msg->direction == MSG_DIR_OUTGOING || msg->direction == MSG_DIR_BROADCAST_OUT);

        char sender[20];
        if (!is_mine) {
            /* Try to get peer name first, fallback to hex address */
            const char* peer_name = mesh_get_peer_name(msg->peer_addr);
            if (peer_name) {
                snprintf(sender, sizeof(sender), "%s", peer_name);
            } else {
                snprintf(sender, sizeof(sender), "%08lX", (unsigned long)msg->peer_addr);
            }
        }

        if (msg_is_action(msg)) {
            add_action_line(s_msg_list, sender, msg, is_mine);
        } else {
            int age_s = (msg->timestamp_s == 0)
                            ? -1
                            : (now_s >= msg->timestamp_s ? (int)(now_s - msg->timestamp_s) : 0);
            add_message_bubble(s_msg_list, is_mine ? NULL : sender, msg, is_mine, age_s);
        }
    }

    if (scroll_to_bottom || was_at_bottom) {
        lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(s_msg_list, prev_y, LV_ANIM_OFF);
    }
}

static void open_with_target(bramble_layout_t* layout, chat_target_t target,
                             int clear_channel_idx) {
    s_target = target;
    layout->in_dm_view = true;
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
    lv_obj_t* header = lv_obj_create(layout->content_area);
    lv_obj_set_size(header, 320, 28);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 40, 24);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_t* back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, back_click_cb, LV_EVENT_CLICKED, layout);

    lv_group_t* g = lv_group_get_default();
    if (g)
        lv_group_add_obj(g, back_btn);

    lv_obj_t* target_btn = lv_btn_create(header);
    lv_obj_set_size(target_btn, 108, 22);
    lv_obj_align(target_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(target_btn, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_border_color(target_btn, BR_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(target_btn, 1, 0);
    lv_obj_t* tgt_lbl = lv_label_create(target_btn);
    lv_label_set_text(tgt_lbl, LV_SYMBOL_REFRESH " channel");
    lv_obj_set_style_text_font(tgt_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(tgt_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_center(tgt_lbl);
    lv_obj_add_event_cb(target_btn, channel_cycle_click_cb, LV_EVENT_CLICKED, NULL);
    if (s_target.kind == CHAT_TARGET_DM) {
        lv_obj_add_flag(target_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (g) {
        lv_group_add_obj(g, target_btn);
    }

    s_title = lv_label_create(header);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_title, BR_COLOR_TEXT, 0);
    lv_obj_align_to(s_title, back_btn, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
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
    lv_obj_set_scroll_dir(s_msg_list, LV_DIR_VER);                /* Prevent horizontal scroll */
    lv_obj_set_scrollbar_mode(s_msg_list, LV_SCROLLBAR_MODE_OFF); /* Hide stray bars */

    /* Load messages from store */
    render_messages_for_target(true);

    /* Compose bar */
    lv_obj_t* compose_bar = lv_obj_create(layout->content_area);
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
    lv_obj_add_event_cb(s_compose_ta, compose_ready_cb, LV_EVENT_READY, NULL); /* Enter key sends */
    lv_obj_set_style_bg_color(s_compose_ta, BR_COLOR_BG, 0);
    lv_obj_set_style_text_color(s_compose_ta, BR_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_compose_ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(s_compose_ta, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
    if (g) {
        lv_group_add_obj(g, s_compose_ta);
        lv_group_focus_obj(s_compose_ta); /* ensure keyboard types into compose bar immediately */
    }

    lv_obj_t* send_btn = lv_btn_create(compose_bar);
    lv_obj_set_size(send_btn, 44, 36);
    lv_obj_set_pos(send_btn, 264, 0);
    lv_obj_set_style_bg_color(send_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_t* send_lbl = lv_label_create(send_btn);
    lv_label_set_text(send_lbl, LV_SYMBOL_OK);
    lv_obj_center(send_lbl);
    lv_obj_add_event_cb(send_btn, send_click_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, send_btn);
}

void scr_chat_messages_open(bramble_layout_t* layout, int channel_idx) {
    chat_target_t target =
        (channel_idx > 0)
            ? chat_target_normalize(CHAT_TARGET_CHANNEL, channel_idx, mesh_get_channel_count())
            : chat_target_default();
    open_with_target(layout, target, channel_idx);
}

void scr_chat_messages_open_dm(bramble_layout_t* layout, uint32_t peer_addr) {
    chat_unread_clear_for_dm(peer_addr);
    open_with_target(layout, chat_target_dm(peer_addr), -1);
}

void scr_chat_messages_on_recv(void) { render_messages_for_target(false); }
