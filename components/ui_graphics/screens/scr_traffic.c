#include "scr_traffic.h"
#include "scr_stats.h"
#include "ui_zone.h"
#include "theme/bramble_theme.h"
#include "traffic_debug.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "scr_traffic";

/* traffic_debug instance lives in main/mesh_task.c, access via extern */
extern traffic_debug_t* mesh_get_traffic_debug(void);

/* Maximum rows to render, keeps memory deterministic and scroll fast */
#define TRAFFIC_DISPLAY_MAX 100

/* -------------------------------------------------------------------------
 * Packet type → short display name (max 5 chars)
 * ------------------------------------------------------------------------- */
static const char* pkt_type_name(uint8_t pkt_type) {
    switch (pkt_type) {
    case 0x01:
        return "ACK";
    case 0x02:
        return "RREQ";
    case 0x03:
        return "RREP";
    case 0x04:
        return "RERR";
    case 0x05:
        return "BCN";
    case 0x06:
        return "KEY";
    case 0x07:
        return "DLVR";
    case 0x08:
        return "CONG";
    case 0x09:
        return "TSYNC";
    case 0x0A:
        return "DATA";
    case 0x0B:
        return "SREQ";
    case 0x0C:
        return "SACK";
    case 0x0D:
        return "MBOX";
    case 0x0E:
        return "MBQ";
    case 0x0F:
        return "EMRG";
    case 0x10:
        return "ECNX";
    case 0x11:
        return "CODE";
    case 0x12:
        return "PROB";
    case 0x13:
        return "PACK";
    case 0x14:
        return "LOC";
    default: {
        static char unk[6];
        snprintf(unk, sizeof(unk), "%02X", pkt_type);
        return unk;
    }
    }
}

/* -------------------------------------------------------------------------
 * Category → short name (max 5 chars)
 * ------------------------------------------------------------------------- */
static const char* cat_name(traffic_category_t cat) {
    switch (cat) {
    case TRAFFIC_CAT_BEACON:
        return "bcn";
    case TRAFFIC_CAT_TIMESYNC:
        return "time";
    case TRAFFIC_CAT_ROUTING:
        return "route";
    case TRAFFIC_CAT_ACK:
        return "ack";
    case TRAFFIC_CAT_CHAT:
        return "chat";
    case TRAFFIC_CAT_MAINTENANCE:
        return "maint";
    case TRAFFIC_CAT_OTHER:
        return "other";
    default:
        return "?";
    }
}

/* -------------------------------------------------------------------------
 * Category → color (theme-matched)
 * ------------------------------------------------------------------------- */
static lv_color_t cat_color(traffic_category_t cat) {
    switch (cat) {
    case TRAFFIC_CAT_CHAT:
        return BR_COLOR_PRIMARY; /* green */
    case TRAFFIC_CAT_ROUTING:
        return BR_COLOR_ACCENT; /* blue  */
    case TRAFFIC_CAT_MAINTENANCE:
        return BR_COLOR_WARNING; /* amber */
    case TRAFFIC_CAT_BEACON:
    case TRAFFIC_CAT_TIMESYNC:
    case TRAFFIC_CAT_ACK:
    case TRAFFIC_CAT_OTHER:
    default:
        return BR_COLOR_TEXT_SEC; /* muted */
    }
}

/* -------------------------------------------------------------------------
 * Row builder: one traffic event
 *
 * Layout (320px content width, 8px side pads → 304px):
 *   Dir  24px | Type  48px | Cat  54px | Size  38px | RSSI  fill
 * ------------------------------------------------------------------------- */
static void create_event_row(lv_obj_t* parent, const traffic_event_t* evt) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 18);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, 0);

    /* Dir: TX (green) or RX (blue) */
    lv_obj_t* dir_lbl = lv_label_create(row);
    lv_obj_set_width(dir_lbl, 24);
    lv_label_set_text(dir_lbl, evt->is_tx ? "TX" : "RX");
    lv_obj_set_style_text_font(dir_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(dir_lbl, evt->is_tx ? BR_COLOR_PRIMARY : BR_COLOR_ACCENT, 0);

    /* Type name */
    lv_obj_t* type_lbl = lv_label_create(row);
    lv_obj_set_width(type_lbl, 48);
    lv_label_set_long_mode(type_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(type_lbl, pkt_type_name(evt->pkt_type));
    lv_obj_set_style_text_font(type_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(type_lbl, BR_COLOR_TEXT, 0);

    /* Category (color coded) */
    lv_obj_t* cat_lbl = lv_label_create(row);
    lv_obj_set_width(cat_lbl, 54);
    lv_label_set_long_mode(cat_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(cat_lbl, cat_name(evt->category));
    lv_obj_set_style_text_font(cat_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cat_lbl, cat_color(evt->category), 0);

    /* Packet size */
    char sz_buf[10];
    snprintf(sz_buf, sizeof(sz_buf), "%ub", evt->packet_len);
    lv_obj_t* sz_lbl = lv_label_create(row);
    lv_obj_set_width(sz_lbl, 38);
    lv_label_set_text(sz_lbl, sz_buf);
    lv_obj_set_style_text_font(sz_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sz_lbl, BR_COLOR_TEXT_SEC, 0);

    /* RSSI (only meaningful for RX; show "--" for TX) */
    char rssi_buf[12];
    if (!evt->is_tx && evt->rssi != 0) {
        snprintf(rssi_buf, sizeof(rssi_buf), "%ddBm", (int)evt->rssi);
    } else {
        strcpy(rssi_buf, "--");
    }
    lv_obj_t* rssi_lbl = lv_label_create(row);
    lv_label_set_text(rssi_lbl, rssi_buf);
    lv_obj_set_style_text_font(rssi_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(
        rssi_lbl, (!evt->is_tx && evt->rssi != 0) ? BR_COLOR_TEXT : BR_COLOR_TEXT_SEC, 0);
}

/* -------------------------------------------------------------------------
 * Back button callback, returns to Stats screen
 * ------------------------------------------------------------------------- */
static lv_obj_t* s_count_lbl = NULL;
static lv_obj_t* s_debug_lbl = NULL;

/* Update only the header event count and the debug-state line in place.
 * Rebuilding the whole screen on a timer would steal trackball focus off
 * the Back button and race the ring buffer harder; the event rows stay a
 * snapshot that refreshes on re-entry. */
static void traffic_refresh_cb(lv_timer_t* timer) {
    (void)timer;
    traffic_debug_t* td = mesh_get_traffic_debug();
    if (!td)
        return;
    if (s_count_lbl)
        lv_label_set_text_fmt(s_count_lbl, "%u evts", traffic_debug_get_count(td));
    if (s_debug_lbl) {
        bool on = traffic_debug_is_enabled(td);
        lv_label_set_text(s_debug_lbl,
                          on ? LV_SYMBOL_BULLET " Debug ON" : LV_SYMBOL_BULLET " Debug OFF");
        lv_obj_set_style_text_color(s_debug_lbl, on ? BR_COLOR_SUCCESS : BR_COLOR_TEXT_SEC, 0);
    }
}

static void traffic_delete_cb2(lv_event_t* e) {
    lv_timer_t* timer = (lv_timer_t*)lv_event_get_user_data(e);
    if (timer)
        lv_timer_delete(timer);
    s_count_lbl = NULL;
    s_debug_lbl = NULL;
}

/* Back lives in the content area scr_stats_create cleans, so the transition runs
 * out of its own click via ui_zone_add_deferred_click. */
static void stats_builder(bramble_layout_t* layout, void* ctx) {
    (void)ctx;
    scr_stats_create(layout);
}

static void back_to_stats_async(void* arg) {
    bramble_layout_t* layout = (bramble_layout_t*)arg;
    if (!layout)
        return;
    lv_refr_now(lv_display_get_default());
    layout_rebuild_content(layout, stats_builder, NULL);
}

/* -------------------------------------------------------------------------
 * Main screen builder
 * ------------------------------------------------------------------------- */
void scr_traffic_create(bramble_layout_t* layout) {
    lv_obj_t* cont = layout_get_content(layout);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);

    traffic_debug_t* td = mesh_get_traffic_debug();
    uint16_t event_count = td ? traffic_debug_get_count(td) : 0;
    bool debug_on = td ? traffic_debug_is_enabled(td) : false;
    uint32_t dropped = td ? traffic_debug_get_dropped(td) : 0;

    ESP_LOGI(TAG, "Traffic monitor: %u events, debug=%d, dropped=%lu", event_count, debug_on,
             (unsigned long)dropped);

    /* ---- Header row: back button + title + event count ---- */
    lv_obj_t* header = lv_obj_create(cont);
    lv_obj_set_size(header, 320, 28);
    lv_obj_set_style_bg_color(header, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(header, 4, 0);

    /* Back button */
    lv_obj_t* back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 48, 20);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back_btn, BR_RADIUS, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_pad_all(back_btn, 2, 0);
    ui_zone_add_deferred_click(back_btn, back_to_stats_async, layout);

    lv_timer_t* refresh = lv_timer_create(traffic_refresh_cb, 2000, NULL);
    lv_obj_add_event_cb(header, traffic_delete_cb2, LV_EVENT_DELETE, refresh);

    lv_obj_t* back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Bk");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(back_lbl, BR_COLOR_TEXT, 0);
    lv_obj_center(back_lbl);

    /* Back is a header action (chrome); the bottom nav stays visible too. It is
     * registered before layout_chrome_tabs_last runs, so it sits at the head of
     * the chrome ring and a hop lands on Back, not on the tab strip. */
    ui_zone_add_chrome(back_btn, true);
    layout_chrome_tabs_last(layout);

    /* Event rows are not focusable, so this screen's content zone is empty. The
     * rebuild's zone reset lands focus in chrome (back / nav) rather than
     * nowhere; no ui_zone_reset_to_content() here (layout_rebuild_content owns
     * it, and this builder only ever runs through it). */

    /* Title */
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Traffic");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Event count badge */
    char cnt_buf[16];
    snprintf(cnt_buf, sizeof(cnt_buf), "%u evts", event_count);
    lv_obj_t* cnt_lbl = lv_label_create(header);
    s_count_lbl = cnt_lbl;
    lv_label_set_text(cnt_lbl, cnt_buf);
    lv_obj_set_style_text_font(cnt_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cnt_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_align(cnt_lbl, LV_ALIGN_RIGHT_MID, -4, 0);

    /* ---- Status line ---- */
    lv_obj_t* status_row = lv_obj_create(cont);
    lv_obj_set_size(status_row, 320, 18);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_row, 0, 0);
    lv_obj_set_style_pad_hor(status_row, BR_PADDING, 0);
    lv_obj_set_style_pad_ver(status_row, 1, 0);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Debug on/off indicator */
    lv_obj_t* dbg_lbl = lv_label_create(status_row);
    s_debug_lbl = dbg_lbl;
    lv_obj_set_style_text_font(dbg_lbl, &lv_font_montserrat_12, 0);
    if (debug_on) {
        /* LV_SYMBOL_BULLET, not U+25CF: only ASCII and the LV_SYMBOL_* block have
         * glyphs in the built-in fonts, so a raw black circle drew a tofu box. */
        lv_label_set_text(dbg_lbl, LV_SYMBOL_BULLET " Debug ON");
        lv_obj_set_style_text_color(dbg_lbl, BR_COLOR_SUCCESS, 0);
    } else {
        lv_label_set_text(dbg_lbl, LV_SYMBOL_BULLET " Debug OFF");
        lv_obj_set_style_text_color(dbg_lbl, BR_COLOR_TEXT_SEC, 0);
    }

    /* Dropped count */
    if (dropped > 0) {
        char drop_buf[24];
        snprintf(drop_buf, sizeof(drop_buf), "%lu dropped", (unsigned long)dropped);
        lv_obj_t* drop_lbl = lv_label_create(status_row);
        lv_label_set_text(drop_lbl, drop_buf);
        lv_obj_set_style_text_font(drop_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(drop_lbl, BR_COLOR_WARNING, 0);
    }

    /* ---- Column header ---- */
    lv_obj_t* col_hdr = lv_obj_create(cont);
    lv_obj_set_size(col_hdr, 320, 16);
    lv_obj_set_style_bg_color(col_hdr, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_bg_opa(col_hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(col_hdr, 0, 0);
    lv_obj_set_style_pad_hor(col_hdr, BR_PADDING, 0);
    lv_obj_set_style_pad_ver(col_hdr, 1, 0);
    lv_obj_clear_flag(col_hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(col_hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(col_hdr, 4, 0);

    const char* hdr_labels[] = {"Dir", "Type", "Cat", "Size", "RSSI"};
    int hdr_widths[] = {24, 48, 54, 38, 0};
    for (int i = 0; i < 5; i++) {
        lv_obj_t* h = lv_label_create(col_hdr);
        if (hdr_widths[i] > 0)
            lv_obj_set_width(h, hdr_widths[i]);
        lv_label_set_text(h, hdr_labels[i]);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(h, BR_COLOR_TEXT_SEC, 0);
    }

    /* ---- Scrollable event list ---- */
    /* Available height: BR_CONTENT_H (180) - 28 header - 18 status - 16 col_hdr = 118px */
    int list_h = BR_CONTENT_H - 28 - 18 - 16;

    lv_obj_t* list = lv_obj_create(cont);
    lv_obj_set_size(list, 320, list_h);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_hor(list, BR_PADDING, 0);
    lv_obj_set_style_pad_ver(list, 2, 0);
    lv_obj_set_style_pad_row(list, 1, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    if (event_count == 0) {
        lv_obj_t* empty = lv_label_create(list);
        if (debug_on) {
            lv_label_set_text(empty, "No events yet.\nPackets will appear as they arrive.");
        } else {
            lv_label_set_text(empty,
                              "Traffic debug is disabled.\nEnable Traffic Debug in Settings.");
        }
        lv_obj_set_style_text_color(empty, BR_COLOR_TEXT_SEC, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(empty);
        return;
    }

    /*
     * Render newest events first (reverse order).
     * Cap at TRAFFIC_DISPLAY_MAX rows to keep LVGL heap usage bounded.
     */
    uint16_t start_idx =
        (event_count > TRAFFIC_DISPLAY_MAX) ? (event_count - TRAFFIC_DISPLAY_MAX) : 0;
    uint16_t render_count = event_count - start_idx;

    for (int i = (int)render_count - 1; i >= 0; i--) {
        const traffic_event_t* evt = traffic_debug_get_event(td, (uint16_t)(start_idx + i));
        if (!evt)
            continue;
        create_event_row(list, evt);
    }
}
