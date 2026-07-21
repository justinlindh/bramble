#include "scr_chat_messages.h"
#include "scr_chat_list.h"
#include "scr_sas_verify.h"
#include "ui_zone.h"
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

/* All four are ui_zone_track'd at creation, so LVGL nulls them the moment the
 * widget dies with the content area. s_msg_list doubles as the authoritative
 * "a chat thread is on screen" flag (see scr_chat_messages_open_target): it is
 * non-NULL exactly while this screen is built. */
static lv_obj_t* s_msg_list = NULL;
static lv_obj_t* s_compose_ta = NULL;
static lv_obj_t* s_send_btn = NULL;
static lv_obj_t* s_title = NULL;
static uint32_t s_selected_packet_id = 0;

/* Key-change interstitial (DM forward-secrecy + SAS, Task 8): which DM this
 * interstitial's buttons act on. Set right before it is shown, read only from
 * its own click callbacks. */
static bramble_layout_t* s_interstitial_layout = NULL;
static uint32_t s_interstitial_peer_addr = 0;

/* Render bound: newest matching messages built per pass. Keeps LVGL
 * object count sane on deep stores. */
#define CHAT_RENDER_MAX 60

static void render_messages_for_target(bool scroll_to_bottom);
static void open_with_target(bramble_layout_t* layout, chat_target_t target, int clear_channel_idx);

/* Use extern for mesh_send: it's in main, not a component */
extern int mesh_send_broadcast(const uint8_t* data, size_t len);
extern uint32_t mesh_send_channel(int channel_idx, uint32_t dest_addr, const uint8_t* data,
                                  size_t len);
extern uint32_t mesh_send_message(uint32_t dest_addr, const uint8_t* data, size_t len);
extern int mesh_get_channel_count(void);
extern const char* mesh_get_channel_name(int index);
extern const char* mesh_get_peer_name(uint32_t addr);
extern bool mesh_get_peer_verification(uint32_t addr, char sas_out[8], bool* verified,
                                       bool* key_changed);

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

/* DM header verify glyph (DM forward-secrecy + SAS, Task 8): three states
 * driven by mesh_get_peer_verification, matching the pager UX spec. No pin at
 * all reads the same as unverified: there is nothing to distinguish for the
 * user until a pin exists. */
static void dm_verify_status(uint32_t peer_addr, const char** text_out, lv_color_t* color_out) {
    char sas[8];
    bool verified = false;
    bool key_changed = false;
    bool have_pin = mesh_get_peer_verification(peer_addr, sas, &verified, &key_changed);

    if (have_pin && key_changed) {
        *text_out = LV_SYMBOL_WARNING " Key changed";
        *color_out = BR_COLOR_DANGER;
    } else if (have_pin && verified) {
        *text_out = LV_SYMBOL_OK " Verified";
        *color_out = BR_COLOR_PRIMARY;
    } else {
        *text_out = LV_SYMBOL_CLOSE " Unverified";
        *color_out = BR_COLOR_TEXT_SEC;
    }
}

/* The verify glyph, Back, and the bubbles all live inside content_area, which
 * the screen they open cleans; the pure transitions register with
 * ui_zone_add_deferred_click (this one re-checks the DM target itself, so a
 * non-DM click safely no-ops). */
static void verify_open_async(void* arg) {
    bramble_layout_t* layout = (bramble_layout_t*)arg;
    if (!layout || s_target.kind != CHAT_TARGET_DM)
        return;
    scr_sas_verify_open(layout, s_target.peer_addr);
}

static void chat_list_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    scr_chat_list_create(layout);
}

static void back_to_list_async(void* arg) {
    bramble_layout_t* layout = (bramble_layout_t*)arg;
    if (!layout)
        return;
    /* Restore tab bar and the content area's normal (tab-bar-visible) size. */
    layout_set_tab_bar_hidden(layout, false);
    lv_obj_set_size(layout->content_area, 320, BR_CONTENT_H);
    lv_obj_set_pos(layout->content_area, 0, BR_STATUS_BAR_H);
    /* Closing the thread: reset the module statics. The rebuild's clean nulls
     * the four tracked widget handles, so the thread reads as closed. */
    s_active_channel = -1;
    s_target = chat_target_default();
    s_selected_packet_id = 0;
    lv_refr_now(lv_display_get_default());
    layout_rebuild_content(layout, chat_list_builder, NULL);
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
        /* Put the cursor back in the compose box so the next message can be typed
         * straight away. Must come AFTER the render: chat_sync_content_group()
         * rebuilds the content group and deliberately restores focus to whatever
         * was focused, which on the Send-button path is the button itself. Both
         * widgets live in the content group, so a plain focus call is enough (no
         * zone switch). No-op on the Enter path, where compose is already focused. */
        if (s_compose_ta)
            lv_group_focus_obj(s_compose_ta);
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

static void rerender_async(void* arg) {
    (void)arg;
    render_messages_for_target(false);
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
    /* The re-render cleans s_msg_list, which owns this very bubble: defer it
     * out of the bubble's own CLICKED dispatch. */
    ui_defer(rerender_async, NULL);
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

        if (i > 0 && !append_text(out, out_len, char_len, " " LV_SYMBOL_RIGHT " ")) {
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
    return snprintf(out, out_len, "%s " LV_SYMBOL_RIGHT " ... " LV_SYMBOL_RIGHT " %s", first_buf,
                    last_buf) > 0;
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

/* Bubble width caps at 220px; a single-line bubble hugs its content instead
 * (fix 3). Padding is 6px each side, so the inner text width available
 * under the cap is 220 - 12. */
#define CHAT_BUBBLE_MAX_W 220
#define CHAT_BUBBLE_MAX_INNER_W (CHAT_BUBBLE_MAX_W - 12)

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
    lv_obj_set_width(bubble, CHAT_BUBBLE_MAX_W); /* provisional; hug pass below may shrink it */
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, is_mine ? BR_COLOR_SENT : BR_COLOR_RECV, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 6, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);

    /* The bubble is this row's focus target for the trackball: navigating the
     * content zone walks bubbles (chat_sync_content_group adds them), a focus
     * border shows which one is selected, and the packet id lets a re-render
     * restore focus to the same message. */
    ui_zone_style_content(bubble);
    lv_obj_set_user_data(bubble, (void*)(uintptr_t)msg->packet_id);

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

    /* Message text + trailing meta ("  <age> <badge>") as one spangroup
     * instead of stacked labels, so a single-line message is one row. LVGL
     * 9.2 dropped label recolor, so spans are the tool for mixed styling.
     * Delivery badge is outgoing-only; received bubbles get age-only meta. */
    char age_buf[8] = "";
    bool have_age = (age_s >= 0);
    if (have_age) {
        chat_format_age((uint32_t)age_s, age_buf, sizeof(age_buf));
    }

    char meta_buf[40] = "";
    lv_color_t meta_color = BR_COLOR_TEXT_SEC;

    if (is_mine) {
        chat_delivery_badge_t badge = chat_message_delivery_badge(msg->status);
        const char* badge_sym = LV_SYMBOL_BULLET;

        if (badge.kind == CHAT_DELIVERY_BADGE_SINGLE_CHECK) {
            badge_sym = LV_SYMBOL_OK;
        } else if (badge.kind == CHAT_DELIVERY_BADGE_DOUBLE_CHECK) {
            badge_sym = LV_SYMBOL_OK " " LV_SYMBOL_OK;
        } else if (badge.kind == CHAT_DELIVERY_BADGE_FAILED) {
            badge_sym = LV_SYMBOL_CLOSE;
        }

        if (badge.color_role == CHAT_DELIVERY_COLOR_DELIVERED) {
            /* Success-green, same green as everywhere else success is signalled.
             * The bubble fill is now blue (BR_COLOR_SENT), so green-on-blue is
             * both visible and semantically consistent. This used to be forced
             * to BR_COLOR_ON_SENT because the bubble was green and green-on-green
             * was invisible; splitting SENT off from the brand green fixed the
             * root cause. */
            meta_color = BR_COLOR_SUCCESS;
        } else if (badge.color_role == CHAT_DELIVERY_COLOR_FAILED) {
            meta_color = BR_COLOR_DANGER;
        }

        /* The multi-hop route is not appended to the meta here: a delivered
         * multi-hop bubble exposes it on demand via the SELECT route toggle
         * (chat_message_has_inline_route_toggle), so the always-on meta stays
         * uncluttered. */

        if (have_age) {
            snprintf(meta_buf, sizeof(meta_buf), "  %s %s", age_buf, badge_sym);
        } else {
            snprintf(meta_buf, sizeof(meta_buf), "  %s", badge_sym);
        }
    } else if (have_age) {
        snprintf(meta_buf, sizeof(meta_buf), "  %s", age_buf);
    }

    lv_obj_t* sg = lv_spangroup_create(bubble);
    lv_obj_set_style_pad_all(sg, 0, 0);

    lv_span_t* text_span = lv_spangroup_new_span(sg);
    lv_span_set_text(text_span, msg->text);
    lv_style_t* text_style = lv_span_get_style(text_span);
    lv_style_set_text_font(text_style, &lv_font_montserrat_14);
    lv_style_set_text_color(text_style, BR_COLOR_TEXT);

    if (meta_buf[0]) {
        lv_span_t* meta_span = lv_spangroup_new_span(sg);
        lv_span_set_text(meta_span, meta_buf);
        lv_style_t* meta_style = lv_span_get_style(meta_span);
        lv_style_set_text_font(meta_style, &lv_font_montserrat_10);
        lv_style_set_text_color(meta_style, meta_color);
    }

    bool can_show_route = chat_message_has_inline_route_toggle(
        is_mine, msg->status, msg->route_hop_count, msg->packet_id);
    bool route_expanded =
        can_show_route && s_selected_packet_id != 0 && msg->packet_id == s_selected_packet_id;

    /* Hug a single line that fits under the cap; wrap (and cap the bubble
     * width) otherwise. lv_spangroup_get_expand_width measures from font
     * metrics alone, so it works before a mode or layout pass. A hugged
     * (LV_SIZE_CONTENT) bubble can't host the route label below, which is
     * LV_PCT(100) width, so route-expanded bubbles always wrap at the cap. */
    uint32_t content_w = lv_spangroup_get_expand_width(sg, 0);
    if (!route_expanded && content_w <= CHAT_BUBBLE_MAX_INNER_W) {
        lv_spangroup_set_mode(sg, LV_SPAN_MODE_EXPAND);
        lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    } else {
        lv_obj_set_width(sg, CHAT_BUBBLE_MAX_INNER_W);
        lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
        lv_obj_set_width(bubble, CHAT_BUBBLE_MAX_W);
    }

    if (can_show_route) {
        if (route_expanded) {
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
}

/* Rebuild the content focus group after the message list is (re)built so the
 * trackball walks bubbles top-to-bottom, then the compose box, then the send
 * button. lv_obj_clean recreates every bubble on each render, so the group is
 * torn down and reassembled each pass; focus is restored to the same widget by
 * identity (compose/send) or by packet id (a bubble), which keeps a route
 * toggle or a fresh arrival from yanking the focus ring. */
static void chat_sync_content_group(void) {
    lv_group_t* cg = ui_zone_content_group();
    if (!cg || !s_msg_list)
        return;

    lv_obj_t* foc = lv_group_get_focused(cg);
    bool foc_compose = (foc && foc == s_compose_ta);
    bool foc_send = (foc && foc == s_send_btn);
    uint32_t foc_pkt =
        (foc && !foc_compose && !foc_send) ? (uint32_t)(uintptr_t)lv_obj_get_user_data(foc) : 0;

    lv_group_remove_all_objs(cg);

    lv_obj_t* restore = NULL;
    uint32_t rows = lv_obj_get_child_count(s_msg_list);
    for (uint32_t i = 0; i < rows; i++) {
        lv_obj_t* row = lv_obj_get_child(s_msg_list, i);
        lv_obj_t* target = lv_obj_get_child(row, 0); /* bubble (or action label) */
        if (!target)
            continue;
        lv_group_add_obj(cg, target);
        if (foc_pkt && (uint32_t)(uintptr_t)lv_obj_get_user_data(target) == foc_pkt)
            restore = target;
    }

    if (s_compose_ta)
        lv_group_add_obj(cg, s_compose_ta);
    if (s_send_btn)
        lv_group_add_obj(cg, s_send_btn);

    if (foc_compose && s_compose_ta)
        restore = s_compose_ta;
    else if (foc_send && s_send_btn)
        restore = s_send_btn;

    if (restore)
        lv_group_focus_obj(restore);
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
        stored_msg_t m;
        if (msg_store_get_copy(i, &m) && message_matches_target(&m))
            match_idx[n_match++] = i;
    }

    for (int k = n_match - 1; k >= 0; k--) {
        /* Copy under the store lock so the mesh task cannot overwrite this
         * slot while we build LVGL objects from it. */
        stored_msg_t local;
        if (!msg_store_get_copy(match_idx[k], &local))
            continue;
        const stored_msg_t* msg = &local;
        if (!message_matches_target(msg))
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

    /* Refresh the content focus group to match the freshly built bubble set
     * before re-asserting the scroll position (focus restore can nudge it). */
    chat_sync_content_group();

    if (scroll_to_bottom || was_at_bottom) {
        lv_obj_scroll_to_y(s_msg_list, LV_COORD_MAX, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(s_msg_list, prev_y, LV_ANIM_OFF);
    }
}

/* Builds the chat thread view (header actions, message list, compose+send) into
 * the freshly cleaned content area. Reads s_target, set by open_with_target
 * before the rebuild. Runs through layout_rebuild_content, which owns the clean
 * and the trailing zone reset. */
static void chat_view_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;

    /* Expand content area to fill space left by the hidden tab bar */
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
    ui_zone_add_deferred_click(back_btn, back_to_list_async, layout);

    /* Header actions (back, channel-switch / verify) are chrome; the message
     * list, compose box, and send button below are content. This view hides the
     * tab bar, so chrome is exactly these header actions, Back first: a hop out
     * of content lands on Back. */
    ui_zone_add_chrome(back_btn, true);

    lv_group_t* g = lv_group_get_default();

    if (s_target.kind == CHAT_TARGET_DM) {
        /* Verify glyph + entry point takes the channel-cycle button's slot:
         * a DM has no channel to cycle. */
        lv_obj_t* verify_btn = lv_btn_create(header);
        lv_obj_set_size(verify_btn, 108, 22);
        lv_obj_align(verify_btn, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_bg_color(verify_btn, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_border_color(verify_btn, BR_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(verify_btn, 1, 0);

        const char* status_text;
        lv_color_t status_color;
        dm_verify_status(s_target.peer_addr, &status_text, &status_color);

        lv_obj_t* verify_lbl = lv_label_create(verify_btn);
        lv_label_set_text(verify_lbl, status_text);
        lv_obj_set_style_text_font(verify_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(verify_lbl, status_color, 0);
        lv_obj_center(verify_lbl);
        ui_zone_add_deferred_click(verify_btn, verify_open_async, layout);
        ui_zone_add_chrome(verify_btn, false);
    } else {
        lv_obj_t* target_btn = lv_btn_create(header);
        lv_obj_set_size(target_btn, 108, 22);
        lv_obj_align(target_btn, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_bg_color(target_btn, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_border_color(target_btn, BR_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(target_btn, 1, 0);
        lv_obj_t* tgt_lbl = lv_label_create(target_btn);
        /* Shuffle glyph reads as "switch conversation", unlike the old refresh
         * glyph that looked like a reload; the title already names the active
         * channel, so the button just needs to signal the switch action. */
        lv_label_set_text(tgt_lbl, LV_SYMBOL_SHUFFLE " Switch");
        lv_obj_set_style_text_font(tgt_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(tgt_lbl, BR_COLOR_TEXT_SEC, 0);
        lv_obj_center(tgt_lbl);
        lv_obj_add_event_cb(target_btn, channel_cycle_click_cb, LV_EVENT_CLICKED, NULL);
        ui_zone_add_chrome(target_btn, false);
    }

    ui_zone_track(&s_title, lv_label_create(header));
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_title, BR_COLOR_TEXT, 0);
    lv_obj_align_to(s_title, back_btn, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    update_title();

    /* Message list area */
    int msg_area_h = content_h - 28 - BR_COMPOSE_BAR_H;
    ui_zone_track(&s_msg_list, lv_obj_create(layout->content_area));
    lv_obj_set_size(s_msg_list, 320, msg_area_h);
    lv_obj_set_pos(s_msg_list, 0, 28);
    lv_obj_set_style_bg_opa(s_msg_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_msg_list, 0, 0);
    lv_obj_set_style_pad_all(s_msg_list, 4, 0);
    lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_msg_list, 4, 0);
    lv_obj_set_scroll_dir(s_msg_list, LV_DIR_VER);                /* Prevent horizontal scroll */
    lv_obj_set_scrollbar_mode(s_msg_list, LV_SCROLLBAR_MODE_OFF); /* Hide stray bars */
    /* content_area itself is non-scrollable (scr_layout.c), but belt and
     * suspenders: never chain a scroll past this list's end. */
    lv_obj_clear_flag(s_msg_list, LV_OBJ_FLAG_SCROLL_CHAIN_VER);

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

    ui_zone_track(&s_compose_ta, lv_textarea_create(compose_bar));
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

    ui_zone_track(&s_send_btn, lv_btn_create(compose_bar));
    lv_obj_set_size(s_send_btn, 44, 36);
    lv_obj_set_pos(s_send_btn, 264, 0);
    lv_obj_set_style_bg_color(s_send_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_t* send_lbl = lv_label_create(s_send_btn);
    lv_label_set_text(send_lbl, LV_SYMBOL_OK);
    lv_obj_center(send_lbl);
    lv_obj_add_event_cb(s_send_btn, send_click_cb, LV_EVENT_CLICKED, NULL);
    if (g)
        lv_group_add_obj(g, s_send_btn);

    /* Keep compose focused so the rebuild's zone reset lights it and the
     * keyboard types into the compose bar immediately. */
    if (s_compose_ta)
        lv_group_focus_obj(s_compose_ta);
}

static void open_with_target(bramble_layout_t* layout, chat_target_t target,
                             int clear_channel_idx) {
    s_target = target;
    s_active_channel = (s_target.kind == CHAT_TARGET_CHANNEL) ? s_target.channel_index : 0;
    s_selected_packet_id = 0;

    if (clear_channel_idx >= 0) {
        chat_unread_clear_for_channel(clear_channel_idx);
    }

    /* Hide tab bar; the rebuild cleans the content area and the builder fills it. */
    layout_set_tab_bar_hidden(layout, true);
    layout_rebuild_content(layout, chat_view_builder, NULL);
}

/* Key-change interstitial (DM forward-secrecy + SAS, Task 8): a genuine
 * identity-key change on this peer was seen and not yet re-verified. Shown
 * before messages on DM open in place of the normal DM view; "Verify Now"
 * goes straight to the SAS screen, "Continue" opens the DM anyway (reading
 * old messages from a since-rotated key is not itself dangerous, only
 * trusting new ones without re-verifying is). */
static void interstitial_verify_async(void* arg) {
    (void)arg;
    if (!s_interstitial_layout)
        return;
    scr_sas_verify_open(s_interstitial_layout, s_interstitial_peer_addr);
}

static void interstitial_continue_async(void* arg) {
    (void)arg;
    if (!s_interstitial_layout)
        return;
    open_with_target(s_interstitial_layout, chat_target_dm(s_interstitial_peer_addr), -1);
}

static void interstitial_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    lv_obj_set_size(layout->content_area, 320, 240 - BR_STATUS_BAR_H);

    lv_obj_t* card = lv_obj_create(layout->content_area);
    lv_obj_set_size(card, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, BR_PADDING, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* warn_lbl = lv_label_create(card);
    lv_label_set_text(warn_lbl, LV_SYMBOL_WARNING " Key Changed");
    lv_obj_set_style_text_font(warn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(warn_lbl, BR_COLOR_DANGER, 0);

    lv_obj_t* msg_lbl = lv_label_create(card);
    lv_label_set_text(msg_lbl, "This contact's key changed. Re-verify.");
    lv_obj_set_width(msg_lbl, lv_pct(100));
    lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(msg_lbl, BR_COLOR_TEXT_SEC, 0);

    lv_obj_t* verify_btn = lv_btn_create(card);
    lv_obj_set_size(verify_btn, 180, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(verify_btn, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_radius(verify_btn, BR_RADIUS, 0);
    lv_obj_t* verify_lbl = lv_label_create(verify_btn);
    lv_label_set_text(verify_lbl, "Verify Now");
    lv_obj_center(verify_lbl);
    ui_zone_add_deferred_click(verify_btn, interstitial_verify_async, NULL);

    lv_obj_t* continue_btn = lv_btn_create(card);
    lv_obj_set_size(continue_btn, 180, BR_TAP_TARGET_MIN);
    lv_obj_set_style_bg_color(continue_btn, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_radius(continue_btn, BR_RADIUS, 0);
    lv_obj_t* continue_lbl = lv_label_create(continue_btn);
    lv_label_set_text(continue_lbl, "Continue");
    lv_obj_center(continue_lbl);
    ui_zone_add_deferred_click(continue_btn, interstitial_continue_async, NULL);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, verify_btn);
        lv_group_add_obj(g, continue_btn);
        lv_group_focus_obj(verify_btn);
    }

    /* Focus persists into the rebuild's zone reset, which lights it. */
    if (g)
        lv_group_focus_obj(verify_btn);
}

static void show_key_change_interstitial(bramble_layout_t* layout, uint32_t peer_addr) {
    s_interstitial_layout = layout;
    s_interstitial_peer_addr = peer_addr;

    /* Hide tab bar; the rebuild cleans the content area and the builder fills it. */
    layout_set_tab_bar_hidden(layout, true);
    layout_rebuild_content(layout, interstitial_builder, NULL);
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

    char sas[8];
    bool verified = false;
    bool key_changed = false;
    if (mesh_get_peer_verification(peer_addr, sas, &verified, &key_changed) && key_changed) {
        show_key_change_interstitial(layout, peer_addr);
        return;
    }

    open_with_target(layout, chat_target_dm(peer_addr), -1);
}

void scr_chat_messages_on_recv(void) { render_messages_for_target(false); }

bool scr_chat_messages_open_target(chat_target_t* out) {
    /* s_msg_list exists only while this screen is built, and LVGL nulls it on
     * teardown, so it answers "is a thread on screen" without any flag to keep
     * in sync. The nav tab cannot answer it: a DM opened from node detail
     * leaves active_tab == TAB_NODES. */
    if (!s_msg_list)
        return false;
    if (out)
        *out = s_target;
    return true;
}
