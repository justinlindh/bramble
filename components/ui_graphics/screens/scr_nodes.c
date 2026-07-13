#include "scr_nodes.h"
#include "scr_node_detail.h"
#include "ui_zone.h"
#include "ui_shared_state.h"
#include "theme/bramble_theme.h"
#include "location.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "scr_nodes";

extern void mesh_get_location_state(location_manager_t* out);

/* Per-card context for drill-down click handler and live refresh */
typedef struct {
    bramble_layout_t* layout;
    neighbor_entry_t neighbor;
    uint32_t now_ms;
    lv_obj_t* info_lbl;
    lv_obj_t* bar;
    lv_obj_t* dot;
} node_card_ctx_t;

#define MAX_CARD_CTX MAX_NEIGHBORS
static node_card_ctx_t s_card_ctx[MAX_CARD_CTX];
static int s_card_ctx_count = 0;

/* Live-refresh state (set by scr_nodes_create, cleared on list delete). */
static lv_obj_t* s_node_list = NULL;
static lv_obj_t* s_node_title = NULL;
static bramble_layout_t* s_node_layout = NULL;
static uint32_t s_node_sig = 0;

static void populate_node_list(void);

/* Cheap membership signature: count plus a rolling hash of addresses.
 * Changes whenever a neighbor appears or is evicted (even count-stable
 * swaps), which is when the card list must be rebuilt rather than aged. */
static uint32_t neighbor_signature(const ui_mesh_state_t* state) {
    uint32_t sig = (uint32_t)state->neighbors.count;
    for (int i = 0; i < state->neighbors.count && i < MAX_NEIGHBORS; i++) {
        sig = sig * 31u + state->neighbors.entries[i].addr;
    }
    return sig;
}

static void format_node_age(char* buf, size_t len, uint32_t age_s) {
    if (age_s < 60)
        snprintf(buf, len, "%lus", (unsigned long)age_s);
    else if (age_s < 3600)
        snprintf(buf, len, "%lum", (unsigned long)(age_s / 60));
    else
        snprintf(buf, len, "%luh", (unsigned long)(age_s / 3600));
}

/* Drill-down target, snapshotted at click time. The clicked card lives in the
 * content area that scr_node_detail_open cleans, so the transition is deferred
 * (see ui_defer); by the time it runs, the live refresh timer may have rebuilt
 * the list and recycled the card's node_card_ctx_t slot, so nothing may be read
 * through that pointer afterwards. */
static struct {
    bramble_layout_t* layout;
    neighbor_entry_t neighbor;
    bool has_loc;
    location_cache_entry_t loc_entry;
    uint32_t now_ms;
} s_pending_open;

static void node_open_async(void* arg) {
    (void)arg;
    if (!s_pending_open.layout)
        return;
    lv_obj_clean(layout_get_content(s_pending_open.layout));
    scr_node_detail_open(s_pending_open.layout, &s_pending_open.neighbor, s_pending_open.has_loc,
                         &s_pending_open.loc_entry, s_pending_open.now_ms);
}

static void node_open_cb(lv_event_t* e) {
    node_card_ctx_t* ctx = (node_card_ctx_t*)lv_event_get_user_data(e);
    if (!ctx || !ctx->layout)
        return;

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

    s_pending_open.layout = ctx->layout;
    s_pending_open.neighbor = ctx->neighbor;
    s_pending_open.has_loc = has_loc;
    s_pending_open.loc_entry = loc_entry;
    s_pending_open.now_ms = ctx->now_ms;
    ui_defer(node_open_async, NULL);
}

static void create_node_card(lv_obj_t* parent, const neighbor_entry_t* n, uint32_t now_ms,
                             bramble_layout_t* layout) {
    lv_obj_t* card = lv_obj_create(parent);
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

    /* Set up drill-down click handler + live-refresh context */
    node_card_ctx_t* ctx = NULL;
    if (s_card_ctx_count < MAX_CARD_CTX) {
        ctx = &s_card_ctx[s_card_ctx_count++];
        ctx->layout = layout;
        ctx->neighbor = *n;
        ctx->now_ms = now_ms;
        lv_obj_add_event_cb(card, node_open_cb, LV_EVENT_CLICKED, ctx);
    }

    lv_group_t* g = lv_group_get_default();
    if (g)
        lv_group_add_obj(g, card);

    /* Node name or address */
    lv_obj_t* name_lbl = lv_label_create(card);
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
    char age_buf[12];
    format_node_age(age_buf, sizeof(age_buf), age_s);
    snprintf(info, sizeof(info), "%ddBm  SNR:%d  %s", n->rssi, n->snr, age_buf);
    lv_obj_t* info_lbl = lv_label_create(card);
    lv_label_set_text(info_lbl, info);
    lv_obj_set_style_text_font(info_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(info_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_width(info_lbl, LV_PCT(68));
    lv_label_set_long_mode(info_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(info_lbl, 0, 20);

    /* Signal bar */
    lv_obj_t* bar = lv_bar_create(card);
    lv_obj_set_size(bar, 40, 8);
    lv_obj_align(bar, LV_ALIGN_TOP_RIGHT, -40, 4);
    lv_obj_set_style_bg_color(bar, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_bg_color(bar, BR_COLOR_SUCCESS, LV_PART_INDICATOR);
    int pct = (n->rssi + 120) * 100 / 70;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);

    /* Online dot */
    bool online = age_s < 600;
    lv_obj_t* dot = lv_obj_create(card);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 0, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, online ? BR_COLOR_SUCCESS : BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);

    if (ctx) {
        ctx->info_lbl = info_lbl;
        ctx->bar = bar;
        ctx->dot = dot;
    }
}

static void nodes_refresh_cb(lv_timer_t* timer) {
    (void)timer;
    const ui_mesh_state_t* state = ui_shared_mesh_state();
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    /* Membership change (peer appeared or was evicted): rebuild the card
     * list so gone nodes disappear and new ones show up, instead of aging
     * a stale set forever. */
    uint32_t sig = neighbor_signature(state);
    if (sig != s_node_sig) {
        s_node_sig = sig;
        populate_node_list();
        return;
    }

    for (int c = 0; c < s_card_ctx_count; c++) {
        node_card_ctx_t* ctx = &s_card_ctx[c];
        if (!ctx->info_lbl)
            continue;
        for (int i = 0; i < state->neighbors.count; i++) {
            const neighbor_entry_t* n = &state->neighbors.entries[i];
            if (n->addr != ctx->neighbor.addr)
                continue;
            ctx->neighbor = *n;
            ctx->now_ms = now_ms;
            uint32_t age_s = (now_ms - n->last_heard) / 1000;
            char age_buf[12];
            format_node_age(age_buf, sizeof(age_buf), age_s);
            lv_label_set_text_fmt(ctx->info_lbl, "%ddBm  SNR:%d  %s", n->rssi, n->snr, age_buf);
            int pct = (n->rssi + 120) * 100 / 70;
            if (pct < 0)
                pct = 0;
            if (pct > 100)
                pct = 100;
            lv_bar_set_value(ctx->bar, pct, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(ctx->dot,
                                      (age_s < 600) ? BR_COLOR_SUCCESS : BR_COLOR_TEXT_SEC, 0);
            break;
        }
    }
}

static void populate_node_list(void) {
    const ui_mesh_state_t* state = ui_shared_mesh_state();
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    int count = state->neighbors.count;

    lv_obj_clean(s_node_list);
    s_card_ctx_count = 0;
    memset(s_card_ctx, 0, sizeof(s_card_ctx));

    char title_buf[32];
    snprintf(title_buf, sizeof(title_buf), "Nodes (%d peer%s)", count, count != 1 ? "s" : "");
    lv_label_set_text(s_node_title, title_buf);

    if (count == 0) {
        lv_obj_t* empty = lv_label_create(s_node_list);
        lv_label_set_text(empty, "No peers discovered yet.\nWaiting for beacons...");
        lv_obj_set_style_text_color(empty, BR_COLOR_TEXT_SEC, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(empty);
        return;
    }

    for (int i = 0; i < count && i < MAX_NEIGHBORS; i++) {
        const neighbor_entry_t* n = &state->neighbors.entries[i];
        if (n->addr == 0)
            continue;
        create_node_card(s_node_list, n, now_ms, s_node_layout);
    }
}

static void nodes_list_delete_cb(lv_event_t* e) {
    lv_timer_t* timer = (lv_timer_t*)lv_event_get_user_data(e);
    if (timer)
        lv_timer_delete(timer);
    s_card_ctx_count = 0;
    s_node_list = NULL;
    s_node_title = NULL;
}

void scr_nodes_create(bramble_layout_t* layout) {
    lv_obj_t* cont = layout_get_content(layout);
    s_node_layout = layout;

    lv_obj_t* title = lv_label_create(cont);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_set_style_pad_left(title, BR_PADDING, 0);
    lv_obj_set_style_pad_top(title, 4, 0);
    s_node_title = title;

    lv_obj_t* list = lv_obj_create(cont);
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
    s_node_list = list;

    s_node_sig = neighbor_signature(ui_shared_mesh_state());
    populate_node_list();

    /* Live refresh every 3 s: ages tick in place; a membership change
     * (new or evicted peer) rebuilds the card list. The timer dies with the
     * list (layout_set_tab cleans the content area). */
    lv_timer_t* refresh = lv_timer_create(nodes_refresh_cb, 3000, NULL);
    lv_obj_add_event_cb(list, nodes_list_delete_cb, LV_EVENT_DELETE, refresh);

    /* Reached from layout_set_tab (which resets the zone) but ALSO directly from
     * the node-detail Back button, so this builder owns the reset too: a screen
     * always leaves input in its content zone. */
    ui_zone_reset_to_content();
}
