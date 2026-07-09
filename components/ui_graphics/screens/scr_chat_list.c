#include "scr_chat_list.h"
#include "theme/bramble_theme.h"
#include "chat_unread.h"
#include "msg_store.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdint.h>

static const char* TAG = "scr_chat";

/* Forward declare — message view and new screens */
extern void scr_chat_messages_open(bramble_layout_t* layout, int channel_idx);
extern void scr_chat_messages_open_dm(bramble_layout_t* layout, uint32_t peer_addr);
extern int mesh_get_channel_count(void);
extern const char* mesh_get_channel_name(int index);
extern const char* mesh_get_peer_name(uint32_t addr);
extern void scr_chat_compose_open(bramble_layout_t* layout);
extern void scr_channel_create_open(bramble_layout_t* layout);

/* UX contract:
 * + Message => open compose flow (broadcast by default)
 * + Channel => open channel creation flow
 */

static void msg_click_cb(lv_event_t* e) {
    bramble_layout_t* layout = (bramble_layout_t*)lv_event_get_user_data(e);
    scr_chat_compose_open(layout);
}

static void channel_click_cb(lv_event_t* e) {
    bramble_layout_t* layout = (bramble_layout_t*)lv_event_get_user_data(e);
    scr_channel_create_open(layout);
}

static void msg_item_click_cb(lv_event_t* e) {
    int channel_idx = (int)(intptr_t)lv_event_get_user_data(e);
    extern bramble_layout_t* s_layout;
    scr_chat_messages_open(s_layout, channel_idx);
}

static void dm_item_click_cb(lv_event_t* e) {
    uint32_t peer_addr = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    extern bramble_layout_t* s_layout;
    scr_chat_messages_open_dm(s_layout, peer_addr);
}

void scr_chat_list_create(bramble_layout_t* layout) {
    lv_obj_t* cont = layout_get_content(layout);

    /* Header row with title and compose button */
    lv_obj_t* header = lv_obj_create(cont);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 28);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Messages");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 4, 0);

    /* Message button */
    lv_obj_t* msg_btn = lv_btn_create(header);
    lv_obj_set_size(msg_btn, 65, 24);
    lv_obj_align(msg_btn, LV_ALIGN_RIGHT_MID, -73, 0);
    lv_obj_set_style_bg_color(msg_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(msg_btn, BR_RADIUS, 0);
    lv_obj_t* msg_lbl = lv_label_create(msg_btn);
    lv_label_set_text(msg_lbl, LV_SYMBOL_EDIT " Msg");
    lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(msg_lbl);
    lv_obj_add_event_cb(msg_btn, msg_click_cb, LV_EVENT_CLICKED, layout);

    /* Channel button */
    lv_obj_t* ch_btn = lv_btn_create(header);
    lv_obj_set_size(ch_btn, 65, 24);
    lv_obj_align(ch_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(ch_btn, BR_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(ch_btn, BR_RADIUS, 0);
    lv_obj_t* ch_lbl = lv_label_create(ch_btn);
    lv_label_set_text(ch_lbl, LV_SYMBOL_PLUS " Ch");
    lv_obj_set_style_text_font(ch_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(ch_lbl);
    lv_obj_add_event_cb(ch_btn, channel_click_cb, LV_EVENT_CLICKED, layout);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, msg_btn);
        lv_group_add_obj(g, ch_btn);
    }

    /* Scrollable message list */
    lv_obj_t* list = lv_obj_create(cont);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, BR_CONTENT_H - 28);
    lv_obj_set_pos(list, 0, 28);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER); /* Prevent horizontal scroll */
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    /* Channel quick-picks */
    int channel_count = mesh_get_channel_count();
    int max_channel = channel_count > 4 ? 4 : channel_count;
    for (int ch = 0; ch < max_channel; ch++) {
        lv_obj_t* card = lv_obj_create(list);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, 36);
        lv_obj_set_style_bg_color(card, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, BR_RADIUS, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(card, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(card, LV_OPA_30, LV_STATE_FOCUSED);
        if (g)
            lv_group_add_obj(g, card);

        lv_obj_t* lbl = lv_label_create(card);
        const char* ch_name = mesh_get_channel_name(ch);
        if (ch_name && ch_name[0]) {
            static char ch_buf[36];
            snprintf(ch_buf, sizeof(ch_buf), "# %s", ch_name);
            lv_label_set_text(lbl, ch_buf);
        } else if (ch == 0) {
            lv_label_set_text(lbl, "# Broadcast");
        } else {
            static char ch_buf[16];
            snprintf(ch_buf, sizeof(ch_buf), "# Channel %d", ch);
            lv_label_set_text(lbl, ch_buf);
        }
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        int unread = chat_unread_count_for_channel(ch);
        if (unread > 0) {
            lv_obj_t* badge = lv_obj_create(card);
            lv_obj_set_size(badge, 26, 18);
            lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_bg_color(badge, BR_COLOR_PRIMARY, 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(badge, 9, 0);
            lv_obj_set_style_border_width(badge, 0, 0);
            lv_obj_set_style_pad_all(badge, 0, 0);
            lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* badge_lbl = lv_label_create(badge);
            lv_label_set_text_fmt(badge_lbl, "%d", unread);
            lv_obj_set_style_text_font(badge_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(badge_lbl, lv_color_white(), 0);
            lv_obj_center(badge_lbl);
        }

        lv_obj_add_event_cb(card, msg_item_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)ch);
    }

    /* DM conversations (peer-based) */
    uint32_t dm_peers[6];
    int dm_count = 0;
    int msg_count = msg_store_count();
    for (int i = 0; i < msg_count && dm_count < 6; i++) {
        const stored_msg_t* m = msg_store_get(i);
        if (!m)
            continue;
        if (m->direction != MSG_DIR_INCOMING && m->direction != MSG_DIR_OUTGOING)
            continue;

        bool exists = false;
        for (int j = 0; j < dm_count; j++) {
            if (dm_peers[j] == m->peer_addr) {
                exists = true;
                break;
            }
        }
        if (!exists)
            dm_peers[dm_count++] = m->peer_addr;
    }

    for (int i = 0; i < dm_count; i++) {
        lv_obj_t* card = lv_obj_create(list);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, 36);
        lv_obj_set_style_bg_color(card, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, BR_RADIUS, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(card);
        const char* peer_name = mesh_get_peer_name(dm_peers[i]);
        static char dm_buf[48];
        if (peer_name && peer_name[0]) {
            snprintf(dm_buf, sizeof(dm_buf), "@ %s", peer_name);
        } else {
            snprintf(dm_buf, sizeof(dm_buf), "@ %08lX", (unsigned long)dm_peers[i]);
        }
        lv_label_set_text(lbl, dm_buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        int dm_unread = chat_unread_count_for_dm(dm_peers[i]);
        if (dm_unread > 0) {
            lv_obj_t* badge = lv_obj_create(card);
            lv_obj_set_size(badge, 26, 18);
            lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_bg_color(badge, BR_COLOR_PRIMARY, 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(badge, 9, 0);
            lv_obj_set_style_border_width(badge, 0, 0);
            lv_obj_set_style_pad_all(badge, 0, 0);
            lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* badge_lbl = lv_label_create(badge);
            lv_label_set_text_fmt(badge_lbl, "%d", dm_unread);
            lv_obj_set_style_text_font(badge_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(badge_lbl, lv_color_white(), 0);
            lv_obj_center(badge_lbl);
        }

        lv_obj_add_event_cb(card, dm_item_click_cb, LV_EVENT_CLICKED,
                            (void*)(uintptr_t)dm_peers[i]);
        if (g)
            lv_group_add_obj(g, card);
    }

    lv_obj_t* hint = lv_label_create(list);
    lv_label_set_text(hint, "Select a channel to view messages.");
    lv_obj_set_style_text_color(hint, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, LV_PCT(100));
}

void scr_chat_list_refresh(bramble_layout_t* layout) { layout_set_tab(layout, TAB_CHAT); }
