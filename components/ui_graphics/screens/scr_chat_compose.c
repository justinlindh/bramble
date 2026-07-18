#include "scr_chat_compose.h"
#include "scr_chat_list.h"
#include "ui_zone.h"
#include "ui_shared_state.h"
#include "theme/bramble_theme.h"
#include "esp_log.h"
#include <stdio.h>

extern void scr_chat_messages_open(bramble_layout_t* layout, int channel_idx);
extern void scr_chat_messages_open_dm(bramble_layout_t* layout, uint32_t peer_addr);
extern int mesh_get_channel_count(void);
extern const char* mesh_get_channel_name(int index);

/* Back and the target rows all sit in the content area their destination
 * cleans, so each transition is registered with ui_zone_add_deferred_click and
 * runs out of its own click. */
extern bramble_layout_t* s_layout;

static void back_to_list_async(void* arg) {
    bramble_layout_t* layout = (bramble_layout_t*)arg;
    if (!layout)
        return;
    /* Show tab bar */
    layout_set_tab_bar_hidden(layout, false);
    /* Return to chat list (layout_set_tab cleans and rebuilds) */
    scr_chat_list_refresh(layout);
}

static void open_channel_async(void* arg) { scr_chat_messages_open(s_layout, (int)(intptr_t)arg); }

static void open_dm_async(void* arg) {
    scr_chat_messages_open_dm(s_layout, (uint32_t)(uintptr_t)arg);
}

static void compose_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;

    /* Expand content area to fill space left by hidden tab bar */
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

    /* Back is a header action (chrome); the target list is content. The tab bar
     * is hidden here, so chrome is just Back: a hop out of content lands on it. */
    ui_zone_add_chrome(back_btn, true);

    lv_group_t* g = lv_group_get_default();

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "New Message");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Scrollable target list */
    lv_obj_t* list = lv_obj_create(layout->content_area);
    lv_obj_set_size(list, 320, content_h - 28);
    lv_obj_set_pos(list, 0, 28);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    /* Add target cards */
    int channel_count = mesh_get_channel_count();

    for (int ch = 0; ch < channel_count; ch++) {
        lv_obj_t* card = lv_obj_create(list);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, 40);
        lv_obj_set_style_bg_color(card, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, BR_RADIUS, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(card, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(card, LV_OPA_30, LV_STATE_FOCUSED);
        if (g)
            lv_group_add_obj(g, card);

        lv_obj_t* lbl = lv_label_create(card);
        const char* ch_name = mesh_get_channel_name(ch);
        if (ch_name && ch_name[0]) {
            lv_label_set_text(lbl, ch_name);
        } else if (ch == 0) {
            lv_label_set_text(lbl, "Broadcast");
        } else {
            static char ch_buf[32];
            snprintf(ch_buf, sizeof(ch_buf), "Channel %d", ch);
            lv_label_set_text(lbl, ch_buf);
        }
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        ui_zone_add_deferred_click(card, open_channel_async, (void*)(intptr_t)ch);
    }

    /* Direct messages: one card per known neighbor */
    const ui_mesh_state_t* mesh = ui_shared_mesh_state();
    for (int i = 0; i < mesh->neighbors.count; i++) {
        uint32_t addr = mesh->neighbors.entries[i].addr;
        if (addr == 0)
            continue;

        lv_obj_t* card = lv_obj_create(list);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, 40);
        lv_obj_set_style_bg_color(card, BR_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, BR_RADIUS, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(card, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(card, LV_OPA_30, LV_STATE_FOCUSED);
        if (g)
            lv_group_add_obj(g, card);

        lv_obj_t* lbl = lv_label_create(card);
        const char* name = mesh->neighbors.entries[i].name;
        char dm_buf[48];
        if (name && name[0]) {
            snprintf(dm_buf, sizeof(dm_buf), "@ %s", name);
        } else {
            snprintf(dm_buf, sizeof(dm_buf), "@ %08lX", (unsigned long)addr);
        }
        lv_label_set_text(lbl, dm_buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        ui_zone_add_deferred_click(card, open_dm_async, (void*)(uintptr_t)addr);
    }

    lv_obj_t* hint = lv_label_create(list);
    lv_label_set_text(hint, "Channels broadcast to members; @ names are direct messages.");
    lv_obj_set_style_text_color(hint, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, LV_PCT(100));
}

void scr_chat_compose_open(bramble_layout_t* layout) {
    /* Hide tab bar; the rebuild cleans the content area and the builder fills it. */
    layout_set_tab_bar_hidden(layout, true);
    layout_rebuild_content(layout, compose_builder, NULL);
}
