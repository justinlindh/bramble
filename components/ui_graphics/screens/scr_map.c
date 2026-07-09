#include "scr_map.h"
#include "ui_shared_state.h"
#include "theme/bramble_theme.h"
#include "location.h"
#include "routing.h"
#include "esp_log.h"
#include <stdio.h>
#include <math.h>

static const char* TAG = "scr_map";
static uint32_t s_focus_peer_addr = 0;

void scr_map_set_focus_peer(uint32_t peer_addr) { s_focus_peer_addr = peer_addr; }

void scr_map_clear_focus_peer(void) { s_focus_peer_addr = 0; }

static bramble_layout_t* s_map_layout = NULL;
static uint32_t s_map_sig = 0;

/* Signature of everything the map draws: self position, focus, and each
 * active peer's position. Rebuild only when it changes, so a stationary
 * mesh does not tear down and rebuild the whole tree every 5 s. */
static uint32_t map_signature(void) {
    const location_manager_t* loc = ui_shared_location_state();
    uint32_t sig = s_focus_peer_addr;
    if (loc->my_position.valid) {
        sig = sig * 31u + (uint32_t)loc->my_position.latitude_e7;
        sig = sig * 31u + (uint32_t)loc->my_position.longitude_e7;
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

static void map_refresh_cb(lv_timer_t* timer) {
    (void)timer;
    if (!s_map_layout)
        return;
    uint32_t sig = map_signature();
    if (sig == s_map_sig)
        return; /* nothing moved; keep the current markers */
    lv_obj_t* cont = layout_get_content(s_map_layout);
    lv_refr_now(lv_display_get_default());
    lv_obj_clean(cont); /* kills the old timer via the DELETE hook */
    scr_map_create(s_map_layout);
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

/* Simple Mercator-like projection helpers */
static void lat_lon_to_pixel(double lat, double lon, double center_lat, double center_lon,
                             double zoom_km, int* px, int* py) {
    /* Simple equirectangular projection (good enough for small areas) */
    /* 1 degree latitude ≈ 111 km */
    /* 1 degree longitude ≈ 111 * cos(lat) km */

    double lat_diff = lat - center_lat;
    double lon_diff = lon - center_lon;

    double km_per_deg_lat = 111.0;
    double km_per_deg_lon = 111.0 * cos(center_lat * M_PI / 180.0);

    double y_km = lat_diff * km_per_deg_lat;
    double x_km = lon_diff * km_per_deg_lon;

    /* Map canvas is 280x140 pixels (leaving margin for labels) */
    double pixels_per_km = 140.0 / zoom_km;

    *px = (int)(140 + x_km * pixels_per_km);
    *py = (int)(70 - y_km * pixels_per_km); /* Invert Y for screen coords */
}

static bool create_marker(lv_obj_t* parent, int x, int y, lv_color_t color, const char* label) {
    if (x < 5 || x >= 275 || y < 5 || y >= 135) {
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
        lv_obj_t* lbl = lv_label_create(parent);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, BR_COLOR_TEXT, 0);
        lv_obj_set_style_bg_color(lbl, BR_COLOR_BG, 0);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_70, 0);
        lv_obj_set_style_pad_all(lbl, 2, 0);
        lv_obj_set_pos(lbl, x + 8, y - 6);
    }
    return true;
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
        /* No GPS data yet */
        lv_obj_t* msg = lv_label_create(cont);
        lv_label_set_text(msg, "No GPS data available.\n\nWaiting for position fix...");
        lv_obj_set_style_text_color(msg, BR_COLOR_TEXT_SEC, 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
        map_arm_refresh(msg); /* keep polling for a fix */
        return;
    }

    /* Calculate map center and bounds */
    double center_lat = self_pos->latitude_e7 / 1e7;
    double center_lon = self_pos->longitude_e7 / 1e7;
    double zoom_km = 5.0; /* Show ±5km area */

    /* Status info */
    char info[128];
    snprintf(info, sizeof(info), "Lat: %.6f  Lon: %.6f  Acc: %um", center_lat, center_lon,
             self_pos->accuracy_m);
    lv_obj_t* info_lbl = lv_label_create(cont);
    lv_label_set_text(info_lbl, info);
    lv_obj_set_style_text_font(info_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(info_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_set_pos(info_lbl, 4, 22);

    /* Create map container */
    lv_obj_t* map_cont = lv_obj_create(cont);
    lv_obj_set_size(map_cont, 312, 148);
    lv_obj_set_pos(map_cont, 4, 38);
    lv_obj_set_style_bg_color(map_cont, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(map_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(map_cont, BR_RADIUS, 0);
    lv_obj_set_style_border_width(map_cont, 1, 0);
    lv_obj_set_style_border_color(map_cont, BR_COLOR_SURFACE_2, 0);
    lv_obj_set_style_pad_all(map_cont, 4, 0);
    lv_obj_clear_flag(map_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* Draw grid crosshair lines using LVGL line objects (no canvas/buffer needed) */
    static lv_point_precise_t h_points[] = {{0, 70}, {280, 70}};
    static lv_point_precise_t v_points[] = {{140, 0}, {140, 140}};

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

    /* Draw self position (blue marker) */
    int self_x, self_y;
    lat_lon_to_pixel(center_lat, center_lon, center_lat, center_lon, zoom_km, &self_x, &self_y);
    create_marker(map_cont, self_x + 4, self_y + 4, lv_color_hex(0x0066FF), "You");

    /* Draw peer positions from cache */
    int peer_count = 0;
    int offmap_count = 0;
    bool focused_peer_has_location = false;
    for (int i = 0; i < loc_state->cache_count && i < LOCATION_MAX_CONTACTS; i++) {
        const location_cache_entry_t* entry = &loc_state->cache[i];
        if (!entry->active || !entry->pos.valid)
            continue;

        double peer_lat = entry->pos.latitude_e7 / 1e7;
        double peer_lon = entry->pos.longitude_e7 / 1e7;

        int px, py;
        lat_lon_to_pixel(peer_lat, peer_lon, center_lat, center_lon, zoom_km, &px, &py);

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

        lv_color_t marker_color =
            (entry->peer_addr == s_focus_peer_addr) ? BR_COLOR_ACCENT : lv_color_hex(0x00CC00);
        bool drawn = create_marker(map_cont, px + 4, py + 4, marker_color, label);
        /* The peer HAS a location (valid cached pos) even if it fell off the
         * visible window; the warning below is only for peers we cannot place
         * at all, not for off-map ones. */
        if (entry->peer_addr == s_focus_peer_addr) {
            focused_peer_has_location = true;
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

    if (s_focus_peer_addr != 0 && !focused_peer_has_location) {
        char focus_info[160];
        snprintf(focus_info, sizeof(focus_info),
                 "Lat: %.6f  Lon: %.6f  Acc: %um  |  %08lX no location", center_lat, center_lon,
                 self_pos->accuracy_m, (unsigned long)s_focus_peer_addr);
        lv_label_set_text(info_lbl, focus_info);
        lv_obj_set_style_text_color(info_lbl, BR_COLOR_WARNING, 0);
    }

    map_arm_refresh(map_cont);

    ESP_LOGI(TAG, "Map created: center=(%.6f, %.6f), peers=%d", center_lat, center_lon, peer_count);
}
