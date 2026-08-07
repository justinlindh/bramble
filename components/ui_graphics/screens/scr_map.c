#include "scr_map.h"
#include "ui_shared_state.h"
#include "ui_zone.h"
#include "map_zoom.h"
#include "theme/bramble_theme.h"
#include "location.h"
#include "gnss_status.h"
#include "routing.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdint.h>
#include <stdio.h>
#include <math.h>

static const char* TAG = "scr_map";
static uint32_t s_focus_peer_addr = 0;

void scr_map_set_focus_peer(uint32_t peer_addr) { s_focus_peer_addr = peer_addr; }

void scr_map_clear_focus_peer(void) { s_focus_peer_addr = 0; }

static bramble_layout_t* s_map_layout = NULL;
static uint32_t s_map_sig = 0;

/* Zoom: an index into the map_zoom ladder, or MAP_ZOOM_AUTO to fit every known
 * peer. Survives rebuilds (static), resets only on reboot. */
static int s_zoom_idx = MAP_ZOOM_AUTO;

/* Right-edge zoom control column, overlaid on the canvas the way every map
 * puts its zoom buttons. Full BR_TAP_TARGET_MIN squares: the touchscreen is
 * how this screen is actually driven, and a control you cannot hit with a
 * thumb is no more usable than the undiscoverable trackball keys were.
 *
 * MAP_CANVAS_W is the inner width of map_cont: 312 wide less its 1 px border
 * and 4 px pad per side, the coordinate space every child sits in. */
#define MAP_CANVAS_W 302
#define ZOOM_BTN_SIZE BR_TAP_TARGET_MIN
#define ZOOM_BTN_X (MAP_CANVAS_W - ZOOM_BTN_SIZE - 2)
#define ZOOM_BTN_Y_IN 2
#define ZOOM_BTN_Y_OUT (ZOOM_BTN_Y_IN + ZOOM_BTN_SIZE + 3)
#define ZOOM_BTN_Y_AUTO (ZOOM_BTN_Y_OUT + ZOOM_BTN_SIZE + 3)
/* Where the scale bar has to stop so it does not run under that column. */
#define SCALE_BAR_RIGHT_X (ZOOM_BTN_X - 8)

/* lv_line keeps the caller's point array; these must outlive the objects.
 * One map instance exists at a time, so single static storage is enough. */
static lv_point_precise_t s_scale_pts[2];
static lv_point_precise_t s_head_pts[2];

/* Simple equirectangular projection (good enough for tens of km).
 * 1 degree latitude ~ 111 km; longitude scaled by cos(lat).
 * Canvas is 280x132 (the container is sized flush to the content area, so
 * its bottom border stays visible; see the map_cont sizing note): the window
 * shows +-zoom_km horizontally, proportionally less vertically. */
static void lat_lon_to_km(double lat, double lon, double center_lat, double center_lon,
                          double* x_km, double* y_km) {
    double km_per_deg_lat = 111.0;
    double km_per_deg_lon = 111.0 * cos(center_lat * M_PI / 180.0);
    *y_km = (lat - center_lat) * km_per_deg_lat;
    *x_km = (lon - center_lon) * km_per_deg_lon;
}

static void km_to_pixel(double x_km, double y_km, double zoom_km, int* px, int* py) {
    double pixels_per_km = (double)MAP_ZOOM_HALF_WIDTH_PX / zoom_km;
    *px = (int)(MAP_ZOOM_HALF_WIDTH_PX + x_km * pixels_per_km);
    *py = (int)(66 - y_km * pixels_per_km); /* Invert Y for screen coords */
}

/* Smallest zoom level whose window contains every active peer (20% margin),
 * or the classic 5 km default when there is nobody to fit. */
static double auto_zoom_km(const location_manager_t* loc, double center_lat, double center_lon) {
    double need = 0.0;
    for (int i = 0; i < loc->cache_count && i < LOCATION_MAX_CONTACTS; i++) {
        const location_cache_entry_t* e = &loc->cache[i];
        if (!e->active || !e->pos.valid)
            continue;
        double x_km, y_km;
        lat_lon_to_km(e->pos.latitude_e7 / 1e7, e->pos.longitude_e7 / 1e7, center_lat, center_lon,
                      &x_km, &y_km);
        double extent = fmax(fabs(x_km), 2.0 * fabs(y_km)); /* vertical half is zoom/2 */
        if (extent > need)
            need = extent;
    }
    if (need <= 0.0)
        return 5.0;
    need *= 1.2;
    return map_zoom_level_km(map_zoom_index_for_km(need));
}

static double resolve_zoom_km(const location_manager_t* loc, double center_lat, double center_lon) {
    if (s_zoom_idx != MAP_ZOOM_AUTO)
        return map_zoom_level_km(s_zoom_idx);
    return auto_zoom_km(loc, center_lat, center_lon);
}

/* What auto would resolve to right now: the rung a step out of auto starts
 * from, and (with no fix) the same 5 km auto_zoom_km falls back to. */
static double current_auto_km(void) {
    const location_manager_t* loc = ui_shared_location_state();
    if (!loc->my_position.valid)
        return 5.0;
    return auto_zoom_km(loc, loc->my_position.latitude_e7 / 1e7,
                        loc->my_position.longitude_e7 / 1e7);
}

/* Signature of everything the map draws: self position, focus, zoom, and each
 * active peer's position. Rebuild only when it changes, so a stationary
 * mesh does not tear down and rebuild the whole tree every 5 s. */
static uint32_t map_signature(void) {
    const location_manager_t* loc = ui_shared_location_state();
    uint32_t sig = s_focus_peer_addr;
    sig = sig * 31u + (uint32_t)(s_zoom_idx + 1);
    if (loc->my_position.valid) {
        sig = sig * 31u + (uint32_t)loc->my_position.latitude_e7;
        sig = sig * 31u + (uint32_t)loc->my_position.longitude_e7;
    } else {
        /* The no-fix panel renders live GNSS state, and without a fix nothing
         * else in this signature moves, so the counts have to be part of it or
         * the panel would freeze on whatever it first drew. They are folded in
         * only on this branch: a satellite count that ticks between 6 and 7
         * must not tear down and rebuild the drawn map. */
        gnss_ui_input_t gnss;
        ui_shared_gnss_state(&gnss);
        sig = sig * 31u + (uint32_t)gnss_ui_classify(&gnss);
        sig = sig * 31u + gnss.sats_in_view;
        sig = sig * 31u + gnss.sats_tracked;
        sig = sig * 31u + gnss.sats_used;
        sig = sig * 31u + gnss.snr_max_dbhz;
    }
    for (int i = 0; i < loc->cache_count && i < LOCATION_MAX_CONTACTS; i++) {
        const location_cache_entry_t* e = &loc->cache[i];
        if (!e->active || !e->pos.valid)
            continue;
        sig = sig * 31u + e->peer_addr;
        sig = sig * 31u + (uint32_t)e->pos.latitude_e7;
        sig = sig * 31u + (uint32_t)e->pos.longitude_e7;
    }
    return sig;
}

static void map_rebuild(void) {
    if (!s_map_layout)
        return;
    lv_obj_t* cont = layout_get_content(s_map_layout);
    lv_refr_now(lv_display_get_default());
    lv_obj_clean(cont); /* kills the old timer via the DELETE hook */
    scr_map_create(s_map_layout);
}

static void map_refresh_cb(lv_timer_t* timer) {
    (void)timer;
    if (!s_map_layout)
        return;
    uint32_t sig = map_signature();
    if (sig == s_map_sig)
        return; /* nothing moved; keep the current markers */
    map_rebuild();
}

static void map_delete_cb(lv_event_t* e) {
    lv_timer_t* timer = (lv_timer_t*)lv_event_get_user_data(e);
    if (timer)
        lv_timer_delete(timer);
}

static void map_arm_refresh(lv_obj_t* owner) {
    lv_timer_t* refresh = lv_timer_create(map_refresh_cb, 5000, NULL);
    lv_obj_add_event_cb(owner, map_delete_cb, LV_EVENT_DELETE, refresh);
}

/* The two zoom mutations, both written to run OUT of LVGL event dispatch.
 *
 * Every route into them (trackball key, on-screen button) ends in a rebuild
 * that lv_obj_clean()s the container holding the widget whose event is still
 * dispatching, and deleting a widget mid-dispatch is a use-after-free that
 * already crashed this screen once. So the state change and the rebuild both
 * live in the deferred half: the key handler calls ui_defer, and the buttons
 * are registered with ui_zone_add_deferred_click, which is the same thing.
 * They rebuild only on a real change, so a press against either end of the
 * ladder costs nothing. */
static void map_zoom_step_async(void* arg) {
    int next = map_zoom_step(s_zoom_idx, (int)(intptr_t)arg, current_auto_km());
    if (next == s_zoom_idx)
        return;
    s_zoom_idx = next;
    map_rebuild();
}

static void map_zoom_auto_async(void* arg) {
    (void)arg;
    if (s_zoom_idx == MAP_ZOOM_AUTO)
        return;
    s_zoom_idx = MAP_ZOOM_AUTO;
    map_rebuild();
}

/* Zoom keys. Trackball UP/DOWN reach us raw because the canvas is flagged
 * UI_ZONE_FLAG_CONSUMES_VERTICAL; +/-/0 come from the keyboard. */
static void map_key_cb(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == '+' || key == '=')
        ui_defer(map_zoom_step_async, (void*)(intptr_t)-1); /* zoom in: smaller window */
    else if (key == LV_KEY_DOWN || key == '-')
        ui_defer(map_zoom_step_async, (void*)(intptr_t)1);
    else if (key == '0' || key == 'f')
        ui_defer(map_zoom_auto_async, NULL);
}

/* Centers of markers we have already given a text label. A later marker that
 * lands within COLLIDE_PX of one of these stacks its label below instead of
 * beside, so nearby dots do not overprint each other's names. Rebuilt every
 * pass (the map tree is torn down and recreated), so no state dangles. */
typedef struct {
    int x;
    int y;
} label_anchor_t;
#define COLLIDE_PX 14
/* One "You" plus up to LOCATION_MAX_CONTACTS peers can be labeled. */
#define MAX_LABEL_ANCHORS (LOCATION_MAX_CONTACTS + 1)

static bool create_marker(lv_obj_t* parent, int x, int y, lv_color_t color, const char* label,
                          label_anchor_t* placed, int* placed_count) {
    if (x < 5 || x >= 275 || y < 5 || y >= 129) {
        return false; /* Off-screen or too close to edge */
    }

    /* Create a simple marker object (small circle) */
    lv_obj_t* marker = lv_obj_create(parent);
    lv_obj_set_size(marker, 10, 10);
    lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(marker, color, 0);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(marker, 2, 0);
    lv_obj_set_style_border_color(marker, lv_color_white(), 0);
    lv_obj_set_style_border_opa(marker, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(marker, 0, 0);
    lv_obj_set_pos(marker, x - 5, y - 5);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_SCROLLABLE);

    /* Draw label */
    if (label) {
        /* Does this marker crowd one that is already labeled? */
        bool below = false;
        if (placed) {
            for (int i = 0; i < *placed_count; i++) {
                int dx = placed[i].x - x;
                int dy = placed[i].y - y;
                if (dx * dx + dy * dy < COLLIDE_PX * COLLIDE_PX) {
                    below = true;
                    break;
                }
            }
        }

        /* Label box width: text plus the 2 px pad on each side. */
        lv_point_t sz;
        lv_text_get_size(&sz, label, &lv_font_montserrat_12, 0, 0, LV_COORD_MAX, 0);
        int box_w = (int)sz.x + 4;

        int lbl_x, lbl_y;
        if (below) {
            /* Stack under the dot to clear the neighbor's beside-label. */
            lbl_x = x - box_w / 2;
            lbl_y = y + 8;
        } else {
            lbl_x = x + 8;
            lbl_y = y - 6;
        }
        /* Edge flip: if the text would run past the right edge, render it to
         * the left of the marker instead of clipping. */
        if (lbl_x + box_w > 275) {
            lbl_x = x - 8 - box_w;
        }
        if (lbl_x < 0) {
            lbl_x = 0;
        }

        lv_obj_t* lbl = lv_label_create(parent);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
        lv_obj_set_style_bg_color(lbl, BR_COLOR_BG, 0);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_70, 0);
        lv_obj_set_style_pad_all(lbl, 2, 0);
        lv_obj_set_pos(lbl, lbl_x, lbl_y);

        if (placed && *placed_count < MAX_LABEL_ANCHORS) {
            placed[*placed_count].x = x;
            placed[*placed_count].y = y;
            (*placed_count)++;
        }
    }
    return true;
}

/* Scale bar, bottom right of the canvas and stopping short of the zoom column.
 * map_zoom picks the round distance and its pixel length; this only draws. */
static void draw_scale_bar(lv_obj_t* map_cont, double zoom_km) {
    int bar_px = 0;
    char txt[16];
    if (!map_zoom_scale_bar(zoom_km, &bar_px, txt, sizeof(txt)))
        return;

    int x1 = SCALE_BAR_RIGHT_X - bar_px;
    s_scale_pts[0] = (lv_point_precise_t){x1, 124};
    s_scale_pts[1] = (lv_point_precise_t){SCALE_BAR_RIGHT_X, 124};
    lv_obj_t* bar = lv_line_create(map_cont);
    lv_line_set_points(bar, s_scale_pts, 2);
    lv_obj_set_style_line_color(bar, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_line_width(bar, 2, 0);

    lv_obj_t* lbl = lv_label_create(map_cont);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_RIGHT, SCALE_BAR_RIGHT_X - MAP_CANVAS_W, -8);
}

/* One overlay zoom control. Not in either focus group and not click-focusable:
 * the content zone still holds exactly the canvas, so tapping a button cannot
 * light a second cursor or hand the trackball a widget it has no way to reach
 * (LEFT/RIGHT always hop to chrome, by design). The trackball's route to zoom
 * is the canvas keys these buttons advertise. */
static void zoom_btn(lv_obj_t* parent, const char* text, const lv_font_t* font, int y, bool enabled,
                     lv_async_cb_t cb, void* ctx) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, ZOOM_BTN_SIZE, ZOOM_BTN_SIZE);
    lv_obj_set_pos(btn, ZOOM_BTN_X, y);
    lv_obj_set_style_radius(btn, BR_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_bg_opa(btn, enabled ? LV_OPA_90 : LV_OPA_40, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, BR_COLOR_BORDER, 0);
    lv_obj_set_style_border_opa(btn, enabled ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, enabled ? BR_COLOR_TEXT : BR_COLOR_TEXT_SEC, 0);
    lv_obj_center(lbl);

    /* A disabled control is drawn, not dropped: the ladder's ends stay visible
     * as ends. Its handler is simply never registered, so a stray tap is inert
     * rather than a no-op rebuild. */
    if (enabled)
        ui_zone_add_deferred_click(btn, cb, ctx);
}

/* The zoom controls plus their readout, drawn last so they sit above the
 * markers. Together they are the whole affordance: the buttons say the map
 * zooms and how, the readout says what it is set to and whether it will
 * re-fit itself when peers move. */
static void draw_zoom_controls(lv_obj_t* map_cont, double zoom_km) {
    double auto_km = current_auto_km();
    zoom_btn(map_cont, "+", &lv_font_montserrat_18, ZOOM_BTN_Y_IN,
             map_zoom_can_step(s_zoom_idx, -1, auto_km), map_zoom_step_async, (void*)(intptr_t)-1);
    zoom_btn(map_cont, "-", &lv_font_montserrat_18, ZOOM_BTN_Y_OUT,
             map_zoom_can_step(s_zoom_idx, 1, auto_km), map_zoom_step_async, (void*)(intptr_t)1);
    zoom_btn(map_cont, "auto", &lv_font_montserrat_12, ZOOM_BTN_Y_AUTO, s_zoom_idx != MAP_ZOOM_AUTO,
             map_zoom_auto_async, NULL);

    char txt[24];
    map_zoom_format(txt, sizeof(txt), s_zoom_idx, zoom_km);
    lv_obj_t* lbl = lv_label_create(map_cont);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_color(lbl, BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(lbl, 2, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 2, 2);
}

static const char* bearing_to_compass(double deg) {
    static const char* WINDS[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int idx = (int)((deg + 22.5) / 45.0) % 8;
    return WINDS[idx];
}

static void format_distance(char* out, size_t out_len, double km) {
    if (km < 1.0)
        snprintf(out, out_len, "%dm", (int)(km * 1000.0));
    else
        snprintf(out, out_len, "%.1fkm", km);
}

void scr_map_create(bramble_layout_t* layout) {
    lv_obj_t* cont = layout_get_content(layout);
    s_map_layout = layout;
    s_map_sig = map_signature();

    /* Get location state */
    const location_manager_t* loc_state = ui_shared_location_state();

    /* Get neighbor state for names */
    const ui_mesh_state_t* mesh_state = ui_shared_mesh_state();

    /* Title */
    lv_obj_t* title = lv_label_create(cont);
    lv_label_set_text(title, LV_SYMBOL_GPS " Map");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, BR_COLOR_TEXT, 0);
    lv_obj_set_style_pad_left(title, BR_PADDING, 0);
    lv_obj_set_style_pad_top(title, 4, 0);

    /* Check if we have valid self position */
    const bramble_position_t* self_pos = &loc_state->my_position;
    bool has_self = self_pos->valid;

    if (!has_self) {
        /* No fix: name which of the three GNSS states the receiver is in, so
         * the screen a field operator opens first tells them whether anything
         * is reaching the antenna at all. */
        gnss_ui_input_t gnss;
        ui_shared_gnss_state(&gnss);
        char detail[48];
        char msg_buf[96];
        gnss_ui_detail_line(&gnss, detail, sizeof(detail));
        snprintf(msg_buf, sizeof(msg_buf), "No position fix.\n\nGNSS: %s\n%s",
                 gnss_ui_state_label(gnss_ui_classify(&gnss)), detail);
        lv_obj_t* msg = lv_label_create(cont);
        lv_label_set_text(msg, msg_buf);
        lv_obj_set_style_text_color(msg, BR_COLOR_TEXT_SEC, 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
        map_arm_refresh(msg); /* keep polling for a fix */
        return;
    }

    /* Calculate map center and bounds */
    double center_lat = self_pos->latitude_e7 / 1e7;
    double center_lon = self_pos->longitude_e7 / 1e7;
    double zoom_km = resolve_zoom_km(loc_state, center_lat, center_lon);
    double pixels_per_km = 140.0 / zoom_km;

    /* Status info (replaced below when a focused peer is placeable) */
    char info[128];
    snprintf(info, sizeof(info), "Lat: %.6f  Lon: %.6f  Acc: %um", center_lat, center_lon,
             self_pos->accuracy_m);
    lv_obj_t* info_lbl = lv_label_create(cont);
    lv_label_set_text(info_lbl, info);
    lv_obj_set_style_text_font(info_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(info_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_pos(info_lbl, 4, 22);

    /* Create map container: a focusable content widget. Trackball UP/DOWN zoom
     * (UI_ZONE_FLAG_CONSUMES_VERTICAL), keyboard +/- too, 0 returns to auto-fit.
     * LEFT/RIGHT still hop to chrome, so the map cannot trap focus. None of
     * that is discoverable, which is what draw_zoom_controls is for; the keys
     * are the shortcut, the buttons are the affordance. */
    lv_obj_t* map_cont = lv_obj_create(cont);
    /* Height fits the content area EXACTLY (y 38 + height = BR_CONTENT_H).
     * The old 148 overflowed the 180px content area by 6px, so the
     * container's bottom edge (and its border, which draws inside the
     * OBJECT but outside the PARENT's clip) rendered under the tab bar:
     * the chopped bottom border this screen shipped with. */
    lv_obj_set_size(map_cont, 312, BR_CONTENT_H - 38);
    lv_obj_set_pos(map_cont, 4, 38);
    lv_obj_set_style_bg_color(map_cont, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(map_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(map_cont, BR_RADIUS, 0);
    lv_obj_set_style_border_width(map_cont, 1, 0);
    lv_obj_set_style_border_color(map_cont, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_pad_all(map_cont, 4, 0);
    lv_obj_clear_flag(map_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(map_cont, UI_ZONE_FLAG_CONSUMES_VERTICAL);
    ui_zone_style_content(map_cont);
    lv_group_add_obj(lv_group_get_default(), map_cont);
    lv_obj_add_event_cb(map_cont, map_key_cb, LV_EVENT_KEY, NULL);
    /* Re-focus after a zoom rebuild, but never steal from chrome: only when
     * the content zone already held the cursor. The sync is not optional
     * either way: adding the canvas to an empty content group auto-focuses it
     * (lv_group_add_obj), so a rebuild that happens while the cursor sits in
     * chrome would otherwise light BOTH the fresh canvas and the chrome tab,
     * and nothing tells the user which one the trackball is driving. */
    if (ui_zone_current() == UI_ZONE_CONTENT) {
        lv_group_focus_obj(map_cont);
    }
    ui_zone_sync_focus_visual();

    /* Draw grid crosshair lines using LVGL line objects (no canvas/buffer needed) */
    static lv_point_precise_t h_points[] = {{0, 66}, {280, 66}};
    static lv_point_precise_t v_points[] = {{140, 0}, {140, 132}};

    lv_obj_t* h_line = lv_line_create(map_cont);
    lv_line_set_points(h_line, h_points, 2);
    lv_obj_set_style_line_color(h_line, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_line_width(h_line, 1, 0);
    lv_obj_set_style_line_opa(h_line, LV_OPA_50, 0);

    lv_obj_t* v_line = lv_line_create(map_cont);
    lv_line_set_points(v_line, v_points, 2);
    lv_obj_set_style_line_color(v_line, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_line_width(v_line, 1, 0);
    lv_obj_set_style_line_opa(v_line, LV_OPA_50, 0);

    /* Accuracy circle around self, only once it is visually meaningful
     * (a 3 m fix at any sane zoom is sub-pixel). */
    int acc_r_px = (int)((double)self_pos->accuracy_m / 1000.0 * pixels_per_km);
    if (acc_r_px >= 4) {
        if (acc_r_px > 66)
            acc_r_px = 66;
        lv_obj_t* acc = lv_obj_create(map_cont);
        lv_obj_set_size(acc, acc_r_px * 2, acc_r_px * 2);
        lv_obj_set_style_radius(acc, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(acc, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(acc, 1, 0);
        lv_obj_set_style_border_color(acc, BR_COLOR_ACCENT, 0);
        lv_obj_set_style_border_opa(acc, LV_OPA_50, 0);
        lv_obj_set_pos(acc, 144 - acc_r_px, 70 - acc_r_px);
        lv_obj_clear_flag(acc, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Heading arrow from the self marker; suppressed when stationary because a
     * GPS course over ground is noise below walking pace. */
    if (self_pos->speed_kmh >= 2) {
        double hdg_rad = (double)(self_pos->heading_deg2 * 2) * M_PI / 180.0;
        s_head_pts[0] = (lv_point_precise_t){144, 70};
        s_head_pts[1] =
            (lv_point_precise_t){144 + (int)(sin(hdg_rad) * 14.0), 70 - (int)(cos(hdg_rad) * 14.0)};
        lv_obj_t* hdg = lv_line_create(map_cont);
        lv_line_set_points(hdg, s_head_pts, 2);
        lv_obj_set_style_line_color(hdg, lv_color_hex(0x0066FF), 0);
        lv_obj_set_style_line_width(hdg, 3, 0);
        lv_obj_set_style_line_rounded(hdg, true, 0);
    }

    /* Label placement bookkeeping: "You" is placed first so peers that crowd
     * the center stack their names below it instead of overprinting. */
    label_anchor_t label_anchors[MAX_LABEL_ANCHORS];
    int label_anchor_count = 0;

    /* At wide zooms every dot's name would collide into mush; show only the
     * focused peer's name (and always "You"), other peers keep just a dot. */
    bool wide_zoom = zoom_km >= 10.0;

    /* Draw self position (blue marker) at the window center */
    create_marker(map_cont, 144, 70, lv_color_hex(0x0066FF), "You", label_anchors,
                  &label_anchor_count);

    /* Draw peer positions from cache */
    int peer_count = 0;
    int offmap_count = 0;
    bool focused_peer_has_location = false;
    bool focused_peer_is_live = false;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    double focus_dist_km = 0.0;
    double focus_bearing_deg = 0.0;
    for (int i = 0; i < loc_state->cache_count && i < LOCATION_MAX_CONTACTS; i++) {
        const location_cache_entry_t* entry = &loc_state->cache[i];
        if (!entry->active || !entry->pos.valid)
            continue;

        double x_km, y_km;
        lat_lon_to_km(entry->pos.latitude_e7 / 1e7, entry->pos.longitude_e7 / 1e7, center_lat,
                      center_lon, &x_km, &y_km);

        int px, py;
        km_to_pixel(x_km, y_km, zoom_km, &px, &py);

        /* Find peer name from neighbor table */
        const char* peer_name = NULL;
        for (int j = 0; j < mesh_state->neighbors.count && j < MAX_NEIGHBORS; j++) {
            if (mesh_state->neighbors.entries[j].addr == entry->peer_addr) {
                if (mesh_state->neighbors.entries[j].name[0]) {
                    peer_name = mesh_state->neighbors.entries[j].name;
                }
                break;
            }
        }

        /* Format label */
        char label[32];
        if (peer_name) {
            snprintf(label, sizeof(label), "%s", peer_name);
        } else {
            snprintf(label, sizeof(label), "%04lX", (unsigned long)(entry->peer_addr & 0xFFFF));
        }

        /* A position restored from flash has no computable age (its timestamp
         * belongs to a boot whose clock is gone), so it is the peer's last
         * known spot, not their current one. Draw it muted so the map does not
         * present it as a live fix. */
        bool live = location_age_is_fresh(entry->age_known, entry->received_ms, now_ms);
        lv_color_t marker_color = (entry->peer_addr == s_focus_peer_addr) ? BR_COLOR_ACCENT
                                  : live                                  ? lv_color_hex(0x00CC00)
                                                                          : lv_color_hex(0x6E7B6E);
        /* Wide-zoom pruning: drop the text for everyone but the focused peer. */
        const char* draw_label =
            (wide_zoom && entry->peer_addr != s_focus_peer_addr) ? NULL : label;
        bool drawn = create_marker(map_cont, px + 4, py + 4, marker_color, draw_label,
                                   label_anchors, &label_anchor_count);
        /* The peer HAS a location (valid cached pos) even if it fell off the
         * visible window; the warning below is only for peers we cannot place
         * at all, not for off-map ones. */
        if (entry->peer_addr == s_focus_peer_addr) {
            focused_peer_has_location = true;
            focused_peer_is_live = live;
            focus_dist_km = sqrt(x_km * x_km + y_km * y_km);
            focus_bearing_deg = atan2(x_km, y_km) * 180.0 / M_PI;
            if (focus_bearing_deg < 0)
                focus_bearing_deg += 360.0;
        }
        if (drawn)
            peer_count++;
        else
            offmap_count++;
    }

    /* Peer count label: only count what is actually drawn */
    char count_str[48];
    if (offmap_count > 0)
        snprintf(count_str, sizeof(count_str), "%d shown (+%d off-map)", peer_count, offmap_count);
    else
        snprintf(count_str, sizeof(count_str), "%d peer%s visible", peer_count,
                 peer_count != 1 ? "s" : "");
    lv_obj_t* count_lbl = lv_label_create(map_cont);
    lv_label_set_text(count_lbl, count_str);
    lv_obj_set_style_text_font(count_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(count_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_bg_color(count_lbl, BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(count_lbl, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(count_lbl, 2, 0);
    lv_obj_align(count_lbl, LV_ALIGN_BOTTOM_LEFT, 2, -2);

    draw_scale_bar(map_cont, zoom_km);
    draw_zoom_controls(map_cont, zoom_km);

    if (s_focus_peer_addr != 0 && focused_peer_has_location) {
        /* Distance + bearing to the person you asked about: the question a
         * mesh pager's map is really answering. */
        char dist_buf[16];
        format_distance(dist_buf, sizeof(dist_buf), focus_dist_km);
        char focus_info[112];
        snprintf(focus_info, sizeof(focus_info), "%08lX: %s %s  |  Acc: %um%s",
                 (unsigned long)s_focus_peer_addr, dist_buf, bearing_to_compass(focus_bearing_deg),
                 self_pos->accuracy_m, focused_peer_is_live ? "" : "  |  last known");
        lv_label_set_text(info_lbl, focus_info);
        lv_obj_set_style_text_color(info_lbl,
                                    focused_peer_is_live ? BR_COLOR_ACCENT : BR_COLOR_TEXT_SEC, 0);
    } else if (s_focus_peer_addr != 0) {
        char focus_info[160];
        snprintf(focus_info, sizeof(focus_info),
                 "Lat: %.6f  Lon: %.6f  Acc: %um  |  %08lX no location", center_lat, center_lon,
                 self_pos->accuracy_m, (unsigned long)s_focus_peer_addr);
        lv_label_set_text(info_lbl, focus_info);
        lv_obj_set_style_text_color(info_lbl, BR_COLOR_WARNING, 0);
    }

    map_arm_refresh(map_cont);

    ESP_LOGI(TAG, "Map created: center=(%.6f, %.6f), zoom=%.1fkm, peers=%d", center_lat, center_lon,
             zoom_km, peer_count);
}
