#include "scr_nodes.h"
#include "scr_node_detail.h"
#include "ui_shared_state.h"
#include "theme/bramble_theme.h"
#include "location.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>

static const char *TAG = "scr_nodes";

extern void mesh_get_location_state(location_manager_t *out);

/* Per-card context for drill-down click handler */
typedef struct {
    bramble_layout_t *layout;
    neighbor_entry_t  neighbor;
    uint32_t          now_ms;
} node_card_ctx_t;

#define MAX_CARD_CTX 16
static node_card_ctx_t s_card_ctx[MAX_CARD_CTX];
static int s_card_ctx_count = 0;

static void node_open_cb(lv_event_t *e) {
    node_card_ctx_t *ctx = (node_card_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->layout) return;

    /* Look up peer location from location cache */
    static location_manager_t loc;
    mesh_get_location_state(&loc);

    bool has_loc = false;
    location_cache_entry_t loc_entry = {0};
    for (int i = 0; i < loc.cache_count && i < LOCATION_MAX_CONTACTS; i++) {
        if (loc.cache[i].active && loc.cache[i].peer_addr == ctx->neighbor.addr) {
            has_loc = true;
            loc_entry = loc.cache[i];
            break;
        }
    }

    lv_obj_clean(layout_get_content(ctx->layout));
    scr_node_detail_open(ctx->layout, &ctx->neighbor, has_loc, &loc_entry, ctx->now_ms);
}

static void create_node_card(lv_obj_t *parent, const neighbor_entry_t *n,
                             uint32_t now_ms, bramble_layout_t *layout) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, 48);
    lv_obj_set_style_bg_color(card, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, BR_RADIUS, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(card, BR_COLOR_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(card, LV_OPA_30, LV_STATE_FOCUSED);

    /* Set up drill-down click handler */
    if (s_card_ctx_count < MAX_CARD_CTX) {
        node_card_ctx_t *ctx = &s_card_ctx[s_card_ctx_count++];
        ctx->layout = layout;
        ctx->neighbor = *n;
        ctx->now_ms = now_ms;
        lv_obj_add_event_cb(card, node_open_cb, LV_EVENT_CLICKED, ctx);
    }

    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_add_obj(g, card);

    /* Node name or address */
    lv_obj_t *name_lbl = lv_label_create(card);
    if (n->name[0]) {
        lv_label_set_text(name_lbl, n->name);
    } else {
        char addr_buf[12];
        snprintf(addr_buf, sizeof(addr_buf), "%08lX", (unsigned long)n->addr);
        lv_label_set_text(name_lbl, addr_buf);
    }
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name_lbl, BR_COLOR_TEXT, 0);
    lv_obj_set_width(name_lbl, LV_PCT(68));
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(name_lbl, 0, 0);

    /* Info line */
    char info[48];
    uint32_t age_s = (now_ms - n->last_heard) / 1000;
    (void)age_s;
    snprintf(info, sizeof(info), "%ddBm  SNR:%d", n->rssi, n->snr);
    lv_obj_t *info_lbl = lv_label_create(card);
    lv_label_set_text(info_lbl, info);
    lv_obj_set_style_text_font(info_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(info_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_width(info_lbl, LV_PCT(68));
    lv_label_set_long_mode(info_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(info_lbl, 0, 20);

    /* Signal bar */
    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, 40, 8);
    lv_obj_align(bar, LV_ALIGN_TOP_RIGHT, -40, 4);
    lv_obj_set_style_bg_color(bar, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_bg_color(bar, BR_COLOR_SUCCESS, LV_PART_INDICATOR);
    int pct = (n->rssi + 120) * 100 / 70;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);

    /* Online dot */
    bool online = age_s < 600;
    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 0, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, online ? BR_COLOR_SUCCESS : BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
}

void scr_nodes_create(bramble_layout_t *layout) {
    lv_obj_t *cont = layout_get_content(layout);

    const ui_mesh_state_t *state = ui_shared_mesh_state();

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    int count = state->neighbors.count;

    /* Reset card context pool */
    s_card_ctx_count = 0;

    char title_buf[32];
    snprintf(title_buf, sizeof(title_buf), "Nodes (%d peer%s)", count, count != 1 ? "s" : "");
    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, title_buf);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_set_style_pad_left(title, BR_PADDING, 0);
    lv_obj_set_style_pad_top(title, 4, 0);

    lv_obj_t *list = lv_obj_create(cont);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, BR_CONTENT_H - 24);
    lv_obj_set_pos(list, 0, 22);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "No peers discovered yet.\nWaiting for beacons...");
        lv_obj_set_style_text_color(empty, BR_COLOR_TEXT_SEC, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(empty);
        return;
    }

    for (int i = 0; i < count && i < MAX_NEIGHBORS; i++) {
        const neighbor_entry_t *n = &state->neighbors.entries[i];
        if (n->addr == 0) continue;
        create_node_card(list, n, now_ms, layout);
    }
}
