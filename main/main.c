#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs_keys.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "display.h"
#include "button.h"
#include "ui.h"
#include "crypto.h"
#include "identity.h"
#include "mesh_task.h"
#include "cli.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include "wifi_manager.h"
#include "ws_server.h"
#include "msg_store.h"
#include "esp_spiffs.h"
#include "mdns.h"
#include "ble_server.h"
#include "esp_system.h"
#include "battery.h"
#include "board_config.h"
#include "keyboard.h"
#include "trackball.h"
#include "location.h"

#include "gps.h"
#include "cJSON.h"

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#include "sdcard.h"
#include "audio.h"
#endif

#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
/* lvgl.h not directly included — use ui_graphics API */
#include "ui_graphics.h"
#endif

static const char *TAG = "bramble";

/* ── Layout constants derived from display size ─────────────────────── */

#define FONT_W          6
#define FONT_H          8
#define LINE_H          (FONT_H + 2)   /* 10px per line */
#define LARGE_FONT_H    (FONT_H * 2)   /* 16px */
#define HEADER_Y        0
#define DIVIDER_Y       (FONT_H + 2)   /* below header text */
#define CONTENT_Y       (DIVIDER_Y + 4) /* below divider */
#define FOOTER_Y        (DISPLAY_HEIGHT - FONT_H - 2)
#define CHARS_PER_LINE  (DISPLAY_WIDTH / FONT_W)

/* ── Location manager ───────────────────────────────────────────────── */

static location_manager_t g_location_mgr;
static bool s_last_gps_fix = false;
static bool s_has_last_wifi_status = false;
static wifi_status_t s_last_wifi_status = {0};

static void emit_gps_event(const char *event, const bramble_position_t *pos) {
    cJSON *params = cJSON_CreateObject();
    if (!params) return;
    cJSON_AddStringToObject(params, "event", event);
    if (pos) {
        cJSON_AddBoolToObject(params, "valid", pos->valid);
        cJSON_AddNumberToObject(params, "lat", pos->latitude_e7 / 1e7);
        cJSON_AddNumberToObject(params, "lon", pos->longitude_e7 / 1e7);
        cJSON_AddNumberToObject(params, "alt_m", pos->altitude_m);
        cJSON_AddNumberToObject(params, "accuracy_m", pos->accuracy_m);
    }
    rpc_notify("bramble.onGpsEvent", params);
    cJSON_Delete(params);
}

static void emit_wifi_event(const wifi_status_t *st, const char *event) {
    cJSON *params = cJSON_CreateObject();
    if (!params) return;
    const bool connected = st->ip_addr[0] != '\0';
    const char *mode = "off";
    if (st->mode == BRAMBLE_WIFI_STATION) mode = "sta";
    else if (st->mode == BRAMBLE_WIFI_AP) mode = "ap";

    cJSON_AddStringToObject(params, "event", event);
    cJSON_AddStringToObject(params, "mode", mode);
    cJSON_AddBoolToObject(params, "connected", connected);
    if (st->ssid[0] != '\0') {
        cJSON_AddStringToObject(params, "ssid", st->ssid);
    }
    cJSON_AddStringToObject(params, "ip", st->ip_addr);
    cJSON_AddNumberToObject(params, "rssi", st->rssi);
    rpc_notify("bramble.onWifiEvent", params);
    cJSON_Delete(params);
}

static void poll_connectivity_events(void) {
    if (board_has_cap(BOARD_CAP_GPS)) {
        bool has_fix = gps_has_fix();
        if (has_fix != s_last_gps_fix) {
            s_last_gps_fix = has_fix;
            if (!has_fix) {
                emit_gps_event("fix_lost", NULL);
            }
        }
    }

    wifi_status_t st;
    wifi_manager_get_status(&st);
    if (!s_has_last_wifi_status) {
        s_last_wifi_status = st;
        s_has_last_wifi_status = true;
        return;
    }
    bool connected = st.ip_addr[0] != '\0';
    bool last_connected = s_last_wifi_status.ip_addr[0] != '\0';
    if (connected != last_connected) {
        emit_wifi_event(&st, connected ? "connected" : "disconnected");
    } else if (strncmp(st.ip_addr, s_last_wifi_status.ip_addr, sizeof(st.ip_addr)) != 0 && connected) {
        emit_wifi_event(&st, "ip_changed");
    }
    s_last_wifi_status = st;
}

static void log_heap_diagnostics_periodic(void)
{
    static uint64_t s_last_log_us = 0;
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (s_last_log_us != 0 && (now_us - s_last_log_us) < 30000000ULL) {
        return;
    }
    s_last_log_us = now_us;

    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t min_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG,
             "diag.heap internal_free=%u internal_min=%u internal_largest=%u psram_free=%u psram_min=%u",
             (unsigned)free_internal,
             (unsigned)min_internal,
             (unsigned)largest_internal,
             (unsigned)free_psram,
             (unsigned)min_psram);
}

static void on_gps_fix(const bramble_position_t *pos, void *ctx) {
    location_manager_t *mgr = (location_manager_t *)ctx;
    location_set_position(mgr, pos);
    if (pos && pos->valid) {
        s_last_gps_fix = true;
        /* Throttle RPC notifications + log to avoid ~60/min of GPS chatter.
         * Internal position tracking (location_set_position above) stays real-time. */
        static uint64_t s_last_gps_notify_us = 0;
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (s_last_gps_notify_us == 0 || (now_us - s_last_gps_notify_us) >= 5000000ULL) {
            s_last_gps_notify_us = now_us;
            emit_gps_event("fix_acquired", pos);
            ESP_LOGI(TAG, "GPS position updated: lat=%.6f lon=%.6f alt=%d",
                     pos->latitude_e7 / 1e7, pos->longitude_e7 / 1e7, pos->altitude_m);
        }
    }
}

/* ── Connectivity mode (NVS-persisted) ──────────────────────────────── */

conn_mode_t conn_mode_get(void) {
    nvs_handle_t nvs;
    /* Default: WiFi only (same as Heltec boards). BLE can be enabled via
     * the Settings screen. Running both on ESP32-S3 exhausts internal SRAM. */
    uint8_t mode = CONN_MODE_WIFI;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, NVS_KEY_CONN_MODE, &mode);
        nvs_close(nvs);
    }
    if (mode == CONN_MODE_BOTH || mode >= CONN_MODE_COUNT) mode = CONN_MODE_WIFI;

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    return conn_mode_resolve_boot((conn_mode_t)mode, true);
#else
    return conn_mode_resolve_boot((conn_mode_t)mode, false);
#endif
}

void conn_mode_set(conn_mode_t mode) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_CONN_MODE, (uint8_t)mode);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Connectivity mode saved: %d", (int)mode);
    }
}

/* ── Splash screen ──────────────────────────────────────────────────── */

#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
/**
 * Show a boot-progress status line on the OLED during initialization.
 * Redraws the BRAMBLE header + divider and places msg below the divider.
 * Only used on non-graphical boards (Heltec V3 SSD1306 OLED).
 */
static void show_boot_status(const char *msg) {
    display_clear();

    /* Redraw BRAMBLE header (same layout as show_splash) */
    int title_w = 7 * FONT_W * 2;
    int title_x = (DISPLAY_WIDTH - title_w) / 2;
    int title_y = DISPLAY_HEIGHT / 4;
    display_draw_text_large(title_x, title_y, "BRAMBLE");

    int div_y = title_y + LARGE_FONT_H + 4;
    display_hline(DISPLAY_WIDTH / 8, div_y, DISPLAY_WIDTH * 3 / 4);

    /* Status line below divider — truncate to display width */
    char buf[22]; /* 128px / 6px-per-char + NUL */
    snprintf(buf, sizeof(buf), "%s", msg);
    display_draw_text(2, div_y + 8, buf);

    display_flush();
}
#endif /* CONFIG_BRAMBLE_UI_GRAPHICAL */

static void show_splash(void) {
    display_clear();

    /* "BRAMBLE" in large text, centered */
    int title_w = 7 * FONT_W * 2;  /* 7 chars × 12px */
    int title_x = (DISPLAY_WIDTH - title_w) / 2;
    int title_y = DISPLAY_HEIGHT / 4;
    display_draw_text_large(title_x, title_y, "BRAMBLE");

    /* Divider line */
    int div_y = title_y + LARGE_FONT_H + 4;
    display_hline(DISPLAY_WIDTH / 8, div_y, DISPLAY_WIDTH * 3 / 4);

    /* Tagline centered */
    const char *tag = "LoRa Mesh Network";
    int tag_w = strlen(tag) * FONT_W;
    display_draw_text((DISPLAY_WIDTH - tag_w) / 2, div_y + 8, tag);

    /* Version centered */
    const char *ver = esp_app_get_description()->version;
    int ver_w = strlen(ver) * FONT_W;
    display_draw_text((DISPLAY_WIDTH - ver_w) / 2, div_y + 20, ver);

    display_flush();
}

/* ── Screen renderers ───────────────────────────────────────────────── */

static bramble_identity_t g_identity;
static uint32_t my_addr = 0;
static uint32_t boot_time_ms = 0;

/* Shared render-time snapshots — only one screen renders at a time,
 * so a single static instance avoids ~8KB of duplicate BSS. */
static mesh_shared_state_t s_render_mesh;
static routing_table_t     s_render_routes;

static void render_main_screen(void) {
    display_clear();

    /* Header — name + battery, right-aligned battery */
    {
        uint8_t bpct = battery_read_pct();
        char name[] = "Bramble";
        display_draw_text(2, HEADER_Y, name);

        char batt[16];
        if (bpct > 0)
            snprintf(batt, sizeof(batt), "%3u%%", bpct);
        else
            snprintf(batt, sizeof(batt), "USB");
        int batt_x = DISPLAY_WIDTH - (strlen(batt) * FONT_W) - 2;
        display_draw_text(batt_x, HEADER_Y, batt);
    }
    display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);

    int y = CONTENT_Y;
    char line[64];

    /* Node address */
    snprintf(line, sizeof(line), "Node: %08" PRIX32, my_addr);
    display_draw_text(2, y, line);
    y += LINE_H;

    /* Get live mesh state */
    mesh_get_state(&s_render_mesh);

    /* Neighbors + radio status */
    int n = neighbor_count(&s_render_mesh.neighbors);
    if (s_render_mesh.radio_ok) {
        snprintf(line, sizeof(line), "Peers: %d", n);
    } else {
        snprintf(line, sizeof(line), "Radio: initializing...");
    }
    display_draw_text(2, y, line);
    y += LINE_H;

    /* WiFi IP address (if connected), else last RX signal */
    const char *ip = wifi_manager_get_ip();
    if (ip && ip[0] != '\0') {
        snprintf(line, sizeof(line), "IP: %s", ip);
    } else if (n > 0) {
        snprintf(line, sizeof(line), "RSSI:%d SNR:%d",
                 s_render_mesh.last_rx_rssi, s_render_mesh.last_rx_snr);
    } else {
        line[0] = '\0';
    }
    if (line[0]) {
        display_draw_text(2, y, line);
    }
    y += LINE_H;

    /* Uptime */
    uint32_t up_sec = (uint32_t)((esp_timer_get_time() / 1000000ULL) -
                                  (boot_time_ms / 1000));
    char uptime[32];
    ui_format_uptime(up_sec, uptime, sizeof(uptime));
    snprintf(line, sizeof(line), "Up: %s", uptime);
    display_draw_text(2, y, line);
    y += LINE_H;

    /* Network reach: direct neighbors + unique routed destinations */
    {
        mesh_get_routes(&s_render_routes);
        int direct = neighbor_count(&s_render_mesh.neighbors);
        int routed = 0;
        for (int i = 0; i < s_render_routes.count; i++) {
            const route_entry_t *r = &s_render_routes.entries[i];
            if (r->state == ROUTE_STALE || r->state == ROUTE_BROKEN) continue;
            bool is_direct = false;
            for (int j = 0; j < s_render_mesh.neighbors.count; j++) {
                if (s_render_mesh.neighbors.entries[j].addr == r->dest_addr) { is_direct = true; break; }
            }
            if (!is_direct) routed++;
        }
        int total = direct + routed;
        snprintf(line, sizeof(line), "Reach:%d (%d direct)", total, direct);
    }
    display_draw_text(2, y, line);

    display_flush();
}

static void render_screen(ui_state_t *ui) {
    switch (ui_get_screen(ui)) {
    case SCREEN_MAIN:
        render_main_screen();
        break;
    case SCREEN_MESSAGES: {
        display_clear();
        int mcount = msg_store_count();
        char hdr[32];
        snprintf(hdr, sizeof(hdr), "Messages (%d)", mcount);
        display_draw_text(2, HEADER_Y, hdr);
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);

        if (mcount == 0) {
            int no_msg_y = (DISPLAY_HEIGHT - FONT_H) / 2;
            const char *no_msg = "(no messages yet)";
            int no_msg_x = (DISPLAY_WIDTH - strlen(no_msg) * FONT_W) / 2;
            display_draw_text(no_msg_x, no_msg_y, no_msg);
        } else {
            /* Calculate how many messages fit */
            int max_msgs = (FOOTER_Y - CONTENT_Y) / LINE_H;
            int start = mcount > max_msgs ? mcount - max_msgs : 0;
            int y = CONTENT_Y;
            for (int i = start; i < mcount && y < FOOTER_Y; i++) {
                const stored_msg_t *m = msg_store_get(i);
                if (!m) continue;
                char line[CHARS_PER_LINE + 1];
                bool outgoing = (m->direction == MSG_DIR_OUTGOING ||
                                 m->direction == MSG_DIR_BROADCAST_OUT);

                char prefix[8];
                if (m->channel_index > 0) {
                    /* Non-default channel: include channel number with direction marker.
                     * Outgoing: "1>", Incoming: "<1" */
                    snprintf(prefix, sizeof(prefix), outgoing ? "%d>" : "<%d", (int)m->channel_index);
                } else {
                    snprintf(prefix, sizeof(prefix), "%s", outgoing ? ">" : "<");
                }

                /* Delivery badge for outgoing messages (2 chars at end) */
                const char *badge = "";
                if (m->direction == MSG_DIR_OUTGOING) {
                    switch (m->status) {
                    case MSG_STATUS_SENT:      badge = " *";  break; /* pending */
                    case MSG_STATUS_DELIVERED:
                        badge = (m->route_hop_count > 1) ? "++" : " +"; break; /* acked */
                    case MSG_STATUS_FAILED:    badge = " x";  break; /* failed */
                    default:                   badge = "";    break;
                    }
                }
                int badge_len = (int)strlen(badge);

                /* Detect CTCP ACTION: \x01ACTION text\x01 */
                bool is_action = (m->text_len > 9 &&
                                  m->text[0] == 0x01 &&
                                  strncmp(m->text + 1, "ACTION ", 7) == 0);

                if (is_action) {
                    /* Extract action text: skip \x01ACTION  (8 bytes), strip trailing \x01 */
                    const char *act = m->text + 8;
                    int act_len = (int)m->text_len - 8;
                    if (act_len > 0 && act[act_len - 1] == 0x01) act_len--;
                    if (act_len < 0) act_len = 0;

                    if (outgoing) {
                        /* "* me <action>" */
                        int act_max = CHARS_PER_LINE - 5 /* "* me " */;
                        if (act_max < 1) act_max = 1;
                        snprintf(line, sizeof(line), "* me %.*s", act_max, act);
                    } else {
                        /* "* XXXX <action>" — last 4 hex digits of peer_addr */
                        int act_max = CHARS_PER_LINE - 7 /* "* XXXX " */;
                        if (act_max < 1) act_max = 1;
                        snprintf(line, sizeof(line), "* %04X %.*s",
                                 (unsigned)(m->peer_addr & 0xFFFF), act_max, act);
                    }
                } else {
                    int text_max = CHARS_PER_LINE - (int)strlen(prefix) - 1 /* space */ - badge_len;
                    if (text_max < 1) text_max = 1;
                    snprintf(line, sizeof(line), "%s %.*s%s", prefix, text_max, m->text, badge);
                }
                display_draw_text(2, y, line);
                y += LINE_H;
            }
        }
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        display_draw_text(2, FOOTER_Y, "[o] compose  < > navigate");
#else
        display_draw_text(2, FOOTER_Y, "[press] next screen");
#endif
        display_flush();
        break;
    }
    case SCREEN_NODES: {
        display_clear();

        mesh_get_state(&s_render_mesh);

        /* Route summary (exclude stale/broken routes for a useful topology snapshot). */
        mesh_get_routes(&s_render_routes);
        int route_count = 0;
        int hop_total = 0;
        for (int i = 0; i < s_render_routes.count; i++) {
            const route_entry_t *r = &s_render_routes.entries[i];
            if (r->dest_addr == 0) continue;
            if (r->state == ROUTE_STALE || r->state == ROUTE_BROKEN) continue;
            route_count++;
            hop_total += r->hop_count;
        }
        int avg_hops_tenths = (route_count > 0) ? ((hop_total * 10) / route_count) : 0;

        char hdr[32];
        snprintf(hdr, sizeof(hdr), "Nodes (%d routes)", route_count);
        display_draw_text(2, HEADER_Y, hdr);
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);

        char summary[32];
        snprintf(summary, sizeof(summary), "Avg: %d.%d hops",
                 avg_hops_tenths / 10, avg_hops_tenths % 10);
        int y = CONTENT_Y;
        display_draw_text(2, y, summary);
        y += LINE_H;

        int cnt = neighbor_count(&s_render_mesh.neighbors);
        if (cnt == 0) {
            int no_nbr_y = y + ((FOOTER_Y - y - FONT_H) / 2);
            if (no_nbr_y < y) no_nbr_y = y;
            const char *no_nbr = "(no neighbors yet)";
            int no_nbr_x = (DISPLAY_WIDTH - strlen(no_nbr) * FONT_W) / 2;
            display_draw_text(no_nbr_x, no_nbr_y, no_nbr);
        } else {
            char nl[64];
            uint32_t now_ms_n = (uint32_t)(esp_timer_get_time() / 1000ULL);
            for (int i = 0; i < s_render_mesh.neighbors.count && y < FOOTER_Y; i++) {
                neighbor_entry_t *e = &s_render_mesh.neighbors.entries[i];
                if (e->addr == 0) continue;
                /* Format last-seen age: seconds, minutes, or hours */
                char age_str[8];
                uint32_t age_ms = (e->last_heard > 0 && now_ms_n >= e->last_heard)
                                  ? (now_ms_n - e->last_heard) : 0;
                uint32_t age_s = age_ms / 1000;
                if (age_s < 60) {
                    snprintf(age_str, sizeof(age_str), "%lus", (unsigned long)age_s);
                } else if (age_s < 3600) {
                    snprintf(age_str, sizeof(age_str), "%lum", (unsigned long)(age_s / 60));
                } else {
                    snprintf(age_str, sizeof(age_str), "%luh+", (unsigned long)(age_s / 3600));
                }
                /* Line: "AABBCCDD -70 12s" (~16 chars, fits 21-char display) */
                snprintf(nl, sizeof(nl), "%08" PRIX32 " %d %s", e->addr, e->rssi, age_str);
                display_draw_text(2, y, nl);
                y += LINE_H;
            }
        }
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        display_draw_text(2, FOOTER_Y, "< > navigate  [o] select");
#else
        display_draw_text(2, FOOTER_Y, "[press] next screen");
#endif
        display_flush();
        break;
    }
    case SCREEN_SETTINGS: {
        display_clear();
        display_draw_text(2, HEADER_Y, "Settings");
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);

        if (ui->settings_editing) {
            int y = CONTENT_Y;
            if (ui->settings_item_cursor == UI_SETTINGS_ITEM_CONN_MODE) {
                display_draw_text(2, y, "Connectivity Mode:");
                y += LINE_H + 4;
                static const char *mode_names[] = {"WiFi", "BLE"};
                conn_mode_t current = conn_mode_get();
                for (int i = 0; i < CONN_MODE_COUNT; i++) {
                    char ml[32];
                    const char *arrow = (i == ui->settings_cursor) ? ">" : " ";
                    const char *mark = (i == (int)current) ? " *" : "";
                    snprintf(ml, sizeof(ml), "%s %s%s", arrow, mode_names[i], mark);
                    display_draw_text(2, y, ml);
                    y += LINE_H;
                }
            } else if (ui->settings_item_cursor == UI_SETTINGS_ITEM_LOCATION) {
                display_draw_text(2, y, "Location Sharing:");
                y += LINE_H + 4;
                static const char *loc_names[] = {"Off", "Coarse", "Exact"};
                loc_share_mode_t cur_loc = location_share_mode_get();
                for (int i = 0; i < LOC_SHARE_COUNT; i++) {
                    char ml[32];
                    const char *arrow = (i == ui->settings_cursor) ? ">" : " ";
                    const char *mark = (i == (int)cur_loc) ? " *" : "";
                    snprintf(ml, sizeof(ml), "%s %s%s", arrow, loc_names[i], mark);
                    display_draw_text(2, y, ml);
                    y += LINE_H;
                }
            } else {
                /* OLED rotation */
                display_draw_text(2, y, "OLED Rotation:");
                y += LINE_H + 4;
                static const char *rot_names[] = {"Normal", "180 deg"};
                bool cur_rot = display_get_rotated_180();
                for (int i = 0; i < 2; i++) {
                    char ml[32];
                    const char *arrow = (i == ui->settings_cursor) ? ">" : " ";
                    const char *mark = (i == (int)cur_rot) ? " *" : "";
                    snprintf(ml, sizeof(ml), "%s %s%s", arrow, rot_names[i], mark);
                    display_draw_text(2, y, ml);
                    y += LINE_H;
                }
            }
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
            display_draw_text(2, FOOTER_Y, "^v choose  [o]OK  [<]cancel");
#else
            display_draw_text(2, FOOTER_Y, "[hold]OK [2x]cancel");
#endif
        } else {
            /* Non-edit: show settings rows with cursor + device info */
            char line[64];
            int y = CONTENT_Y;

            conn_mode_t cur_mode = conn_mode_get();
            static const char *mnames[] = {"WiFi", "BLE"};
            loc_share_mode_t cur_loc = location_share_mode_get();
            static const char *loc_names[] = {"Off", "Coarse", "Exact"};

            /* Row 0: Connectivity */
            {
                const char *sel = (ui->settings_item_cursor == UI_SETTINGS_ITEM_CONN_MODE) ? ">" : " ";
                snprintf(line, sizeof(line), "%sConn: %s", sel, mnames[cur_mode]);
                display_draw_text(2, y, line);
                y += LINE_H;
            }
            /* Row 1: OLED Rotation (placeholder) */
            {
                const char *sel = (ui->settings_item_cursor == UI_SETTINGS_ITEM_OLED_ROTATION) ? ">" : " ";
                snprintf(line, sizeof(line), "%sRotation: 0", sel);
                display_draw_text(2, y, line);
                y += LINE_H;
            }
            /* Row 2: Location Sharing */
            {
                const char *sel = (ui->settings_item_cursor == UI_SETTINGS_ITEM_LOCATION) ? ">" : " ";
                snprintf(line, sizeof(line), "%sLocation: %s", sel, loc_names[cur_loc]);
                display_draw_text(2, y, line);
                y += LINE_H;
            }
            /* Device info row */
            snprintf(line, sizeof(line), "Addr: %08" PRIX32, my_addr);
            display_draw_text(2, y, line);
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
            display_draw_text(2, FOOTER_Y, "[o] edit  < > navigate");
#else
            display_draw_text(2, FOOTER_Y, "[press]next [hold]edit");
#endif
        }
        display_flush();
        break;
    }
    case SCREEN_COMPOSE: {
        display_clear();
        
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        /* T-Deck Plus: full compose screen with keyboard input */
        display_draw_text(2, HEADER_Y, "Compose");
        
        /* Show recipient (broadcast for now) */
        char recip[] = "To: Broadcast";
        int recip_x = DISPLAY_WIDTH - (strlen(recip) * FONT_W) - 2;
        display_draw_text(recip_x, HEADER_Y, recip);
        
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);
        
        /* Message text area */
        int y = CONTENT_Y;
        int chars_per_row = CHARS_PER_LINE - 1;  /* leave margin */
        
        /* Word-wrap compose buffer */
        for (int i = 0; i < ui->compose_len && y < FOOTER_Y - LINE_H; ) {
            char row[64];
            int row_len = (ui->compose_len - i > chars_per_row) ? chars_per_row : ui->compose_len - i;
            memcpy(row, ui->compose_buf + i, row_len);
            row[row_len] = '\0';
            display_draw_text(2, y, row);
            i += row_len;
            y += LINE_H;
        }
        
        /* Cursor (blinking underscore after text) */
        int cursor_x = 2 + (ui->compose_len % chars_per_row) * FONT_W;
        int cursor_y = CONTENT_Y + (ui->compose_len / chars_per_row) * LINE_H;
        if (cursor_y < FOOTER_Y - LINE_H) {
            display_draw_text(cursor_x, cursor_y, "_");
        }
        
        /* Footer */
        display_draw_text(2, FOOTER_Y, "[Enter] Send  [Esc/Left] Back");
#else
        /* Heltec: Stats screen */
        display_draw_text(2, HEADER_Y, "Stats  v0.1");
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);

        char line[64];
        int y = CONTENT_Y;

        /* Line 1: TX/RX counters */
        mesh_get_state(&s_render_mesh);
        snprintf(line, sizeof(line), "TX:%"PRIu32" RX:%"PRIu32,
                 s_render_mesh.packets_tx, s_render_mesh.packets_rx);
        display_draw_text(2, y, line);
        y += LINE_H;

        /* Line 2: Airtime TX budget used % (NORMAL tier) */
        {
            uint32_t used_ms = 0;
            uint32_t max_ms  = s_render_mesh.airtime.max_ms[0];
            if (max_ms > 0 && s_render_mesh.airtime.tokens_ms[0] <= max_ms) {
                used_ms = max_ms - s_render_mesh.airtime.tokens_ms[0];
            }
            uint32_t pct_tenths = (max_ms > 0) ? (used_ms * 1000 / max_ms) : 0;
            snprintf(line, sizeof(line), "Air: %"PRIu32".%"PRIu32"%% TX",
                     pct_tenths / 10, pct_tenths % 10);
        }
        display_draw_text(2, y, line);
        y += LINE_H;

        /* Line 3: Heap + Uptime */
        {
            uint32_t heap_kb = esp_get_free_heap_size() / 1024;
            uint32_t up_sec = (uint32_t)((esp_timer_get_time() / 1000000ULL) -
                                          (boot_time_ms / 1000));
            char uptime[20];
            ui_format_uptime(up_sec, uptime, sizeof(uptime));
            snprintf(line, sizeof(line), "Heap:%"PRIu32"KB Up:%s", heap_kb, uptime);
        }
        display_draw_text(2, y, line);
        y += LINE_H;

        /* Line 4: Routes + Battery */
        {
            mesh_get_routes(&s_render_routes);
            int route_cnt = 0;
            for (int i = 0; i < s_render_routes.count; i++) {
                route_state_t st = s_render_routes.entries[i].state;
                if (st != ROUTE_STALE && st != ROUTE_BROKEN) route_cnt++;
            }
            uint8_t bpct = battery_read_pct();
            snprintf(line, sizeof(line), "Routes:%d Batt:%u%%", route_cnt, (unsigned)bpct);
        }
        display_draw_text(2, y, line);

        display_draw_text(2, FOOTER_Y, "[press] next screen");
#endif
        
        display_flush();
        break;
    }
    default:
        display_clear();
        display_draw_text(0, 28, "Unknown screen");
        display_flush();
        break;
    }
}

/* ── Main ───────────────────────────────────────────────────────────── */

#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
static void lv_tick_cb(void *arg) {
    (void)arg;
    ui_graphics_tick_1ms();
}

static void ui_graphics_task(void *arg) {
    (void)arg;
    while (1) {
        uint32_t delay = ui_graphics_tick();
        if (delay < 5) delay = 5;
        if (delay > 30) delay = 30;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "=== BOOT STAGE: app_main entry ===");
    ESP_LOGI(TAG, "Bramble LoRa Mesh starting...");

    /* Board-level init: power rails, shared SPI bus */
    ESP_LOGI(TAG, "=== BOOT STAGE: board_init ===");
    if (board_init() != 0) {
        ESP_LOGE(TAG, "Board init failed — halting");
        return;
    }

    /* NVS init */
    ESP_LOGI(TAG, "=== BOOT STAGE: nvs_flash_init ===");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated/new version — erasing and reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    /* Load or generate persistent identity */
    ESP_LOGI(TAG, "=== BOOT STAGE: identity_load ===");
    if (identity_load(&g_identity) == 0) {
        ESP_LOGI(TAG, "Identity loaded from NVS");
    } else {
        ESP_LOGI(TAG, "No identity found, generating new keypair...");
        ESP_LOGI(TAG, "=== BOOT STAGE: identity_generate_and_save ===");
        if (identity_generate_and_save(&g_identity) != 0) {
            ESP_LOGE(TAG, "Identity generation failed!");
            /* Fallback to random address */
            uint8_t addr_bytes[4];
            crypto_random(addr_bytes, 4);
            g_identity.address = (uint32_t)(addr_bytes[0] | (addr_bytes[1] << 8) |
                                            (addr_bytes[2] << 16) | (addr_bytes[3] << 24));
        }
    }
    my_addr = g_identity.address;
    ESP_LOGI(TAG, "Node address: %08" PRIX32 " (pubkey hash: %08" PRIX32 ")",
             my_addr, g_identity.pubkey_hash);

    boot_time_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    /* Init display */
    ESP_LOGI(TAG, "=== BOOT STAGE: display_init ===");
    if (display_init() != 0) {
        ESP_LOGE(TAG, "Display init failed!");
    } else {
        /* Restore saved OLED rotation from NVS */
        {
            nvs_handle_t nvs;
            if (nvs_open(NVS_NS_BRAMBLE, NVS_READONLY, &nvs) == ESP_OK) {
                uint8_t rot = 0;
                if (nvs_get_u8(nvs, NVS_KEY_OLED_ROT, &rot) == ESP_OK && rot) {
                    display_set_rotated_180(true);
                    ESP_LOGI(TAG, "OLED rotation restored: 180 deg");
                }
                nvs_close(nvs);
            }
        }
#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
        /* LVGL will handle its own rendering — just clear the display */
        ESP_LOGI(TAG, "=== BOOT STAGE: clear display for LVGL ===");
        display_clear();
        display_flush();
#else
        ESP_LOGI(TAG, "=== BOOT STAGE: show_splash ===");
        show_splash();
        ESP_LOGI(TAG, "Splash screen displayed");
        vTaskDelay(pdMS_TO_TICKS(500)); /* Brief pause so splash is visible */
        show_boot_status("Initializing...");
#endif
    }

    /* Init button */
    ESP_LOGI(TAG, "=== BOOT STAGE: button_init ===");
    button_init();

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    /* Init keyboard and trackball (T-Deck Plus only) */
    ESP_LOGI(TAG, "=== BOOT STAGE: keyboard_init ===");
    keyboard_init();
    keyboard_set_backlight(255);  /* Keyboard LEDs at full brightness */
    ESP_LOGI(TAG, "=== BOOT STAGE: trackball_init ===");
    trackball_init();
#endif

    ESP_LOGI(TAG, "=== BOOT STAGE: battery_init ===");
    battery_init();
    ESP_LOGI(TAG, "Battery: %" PRIu32 " mV (%u%%)", battery_read_mv(), battery_read_pct());

    /* Init location manager */
    ESP_LOGI(TAG, "=== BOOT STAGE: location_init ===");
    location_init(&g_location_mgr);

    /* Init GPS on boards that advertise GPS capability */
    if (board_has_cap(BOARD_CAP_GPS)) {
        ESP_LOGI(TAG, "=== BOOT STAGE: gps_init ===");
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
        show_boot_status("GPS: searching...");
#endif
        if (gps_init(on_gps_fix, &g_location_mgr) == 0) {
            ESP_LOGI(TAG, "GPS initialized (waiting for fix...)");
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
            show_boot_status("GPS: ok (no fix yet)");
#endif
        } else {
            ESP_LOGW(TAG, "GPS init failed or not available");
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
            show_boot_status("GPS: fail");
#endif
        }
    } else {
        ESP_LOGI(TAG, "GPS not supported on this board");
    }

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    /* Init SD card (T-Deck Plus only) */
    ESP_LOGI(TAG, "=== BOOT STAGE: sdcard_init ===");
    if (sdcard_init() == 0) {
        ESP_LOGI(TAG, "SD card mounted at %s", sdcard_get_mount_point());
    } else {
        ESP_LOGW(TAG, "SD card init failed or not present");
    }

    /* Init audio (T-Deck Plus only) */
    ESP_LOGI(TAG, "=== BOOT STAGE: audio_init ===");
    if (board_has_cap(BOARD_CAP_AUDIO)) {
        if (audio_init() == 0) {
            ESP_LOGI(TAG, "Audio initialized");
            audio_play_tone(AUDIO_TONE_BOOT);
        } else {
            ESP_LOGW(TAG, "Audio init failed");
        }
    }
#endif

    /* Read connectivity mode */
    conn_mode_t boot_mode = conn_mode_get();
    static const char *mode_str[] = {"WiFi", "BLE"};
    ESP_LOGI(TAG, "Connectivity mode: %s", mode_str[boot_mode]);

    /* Init RPC dispatcher and register methods BEFORE transports
     * so ws_server/ble notify registrations aren't wiped by rpc_init() */
    ESP_LOGI(TAG, "=== BOOT STAGE: rpc_init ===");
    rpc_init();
    rpc_methods_init(&g_identity);

#if CONFIG_BRAMBLE_MSG_PERSIST_ENABLED
    /* Mount SPIFFS for persisted message storage */
    ESP_LOGI(TAG, "=== BOOT STAGE: spiffs_mount ===");
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t spiffs_ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (spiffs_ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (%s), msg persistence disabled", esp_err_to_name(spiffs_ret));
    } else {
        size_t total = 0, used = 0;
        if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
            ESP_LOGI(TAG, "SPIFFS mounted: used=%u / total=%u bytes", (unsigned)used, (unsigned)total);
        }
    }

    /* Init message store with persistence restore */
    msg_store_init_with_persistence();
#else
    /* Init message store (RAM-only) */
    msg_store_init();
#endif

    /* Init WiFi if selected */
    if (boot_mode == CONN_MODE_WIFI) {
        ESP_LOGI(TAG, "=== BOOT STAGE: wifi_init ===");
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
        show_boot_status("WiFi: starting...");
#endif
        if (wifi_manager_init(my_addr) == 0) {
            const char *ip = wifi_manager_get_ip();
            if (ip[0] != '\0') {
                ESP_LOGI(TAG, "WiFi ready: %s", ip);
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
                {
                    char wifi_status_line[22];
                    snprintf(wifi_status_line, sizeof(wifi_status_line), "WiFi: %s", ip);
                    show_boot_status(wifi_status_line);
                }
#endif

                ESP_LOGI(TAG, "=== BOOT STAGE: ws_server_start ===");
                ws_server_start();

                ESP_LOGI(TAG, "=== BOOT STAGE: mdns_init ===");
                mdns_init();
                char hostname[32];
                snprintf(hostname, sizeof(hostname), "bramble-%04" PRIx32, my_addr & 0xFFFF);
                mdns_hostname_set(hostname);
                mdns_instance_name_set("Bramble Mesh Node");
                mdns_service_add("Bramble", "_bramble", "_tcp", 80, NULL, 0);
                ESP_LOGI(TAG, "mDNS: %s._bramble._tcp", hostname);
            } else {
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
                show_boot_status("WiFi: AP 192.168.4.1");
#endif
            }
        } else {
            ESP_LOGW(TAG, "WiFi init failed");
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
            show_boot_status("WiFi: fail");
#endif
        }
    } else {
        ESP_LOGI(TAG, "WiFi disabled by connectivity mode");
    }

    /* Start mesh task (radio + beacons on CPU1).
     * NOTE: radio_init() runs inside mesh_task on CPU1 — if it hangs,
     * the task watchdog (CONFIG_ESP_TASK_WDT_TIMEOUT_S) will force a reset. */
    ESP_LOGI(TAG, "=== BOOT STAGE: mesh_task_start ===");
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
    show_boot_status("Radio: SX1262...");
#endif
    mesh_task_start(&g_identity);
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
    /* Radio init runs async in mesh_task — brief wait then check state */
    vTaskDelay(pdMS_TO_TICKS(800));
    {
        static mesh_shared_state_t boot_mesh;
        mesh_get_state(&boot_mesh);
        if (boot_mesh.radio_ok) {
            show_boot_status("Radio: OK  Mesh: active");
        } else {
            show_boot_status("Radio: init...  Mesh: wait");
        }
    }
#endif

    /* Start BLE GATT server if selected */
    if (boot_mode == CONN_MODE_BLE) {
        ESP_LOGI(TAG, "=== BOOT STAGE: ble_init ===");
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
        show_boot_status("BLE: starting...");
#endif
        if (ble_server_init() == 0) {
            ble_server_start();
            ESP_LOGI(TAG, "BLE server started");
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
            show_boot_status("BLE: advertising");
#endif
        } else {
            ESP_LOGW(TAG, "BLE init failed");
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
            show_boot_status("BLE: fail");
#endif
        }
    } else {
        ESP_LOGI(TAG, "BLE disabled by connectivity mode");
    }

    /* Start serial CLI (with JSON-RPC auto-detect) */
    ESP_LOGI(TAG, "=== BOOT STAGE: cli_init ===");
    cli_init(&g_identity);

#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
    /* Initialize LVGL graphical UI */
    ESP_LOGI(TAG, "=== BOOT STAGE: ui_graphics_init ===");
    ui_graphics_init();
    
    /* Create 1ms tick timer for LVGL */
    const esp_timer_create_args_t tick_args = {
        .callback = lv_tick_cb,
        .name = "lv_tick"
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);
    
    /* Create LVGL task on core 1 (core 0 runs mesh).
     *
     * IMPORTANT constraints:
     * 1. Stack MUST be in internal RAM — LVGL callbacks (Settings screen)
     *    do NVS reads which trigger SPI flash operations. ESP-IDF asserts
     *    the calling task's stack is cache-safe (internal RAM) before
     *    disabling caches. PSRAM stack → assert crash.
     * 2. Internal RAM headroom can dip below ~24KB after connectivity init.
     *    Keep ui_gfx stack conservative to ensure task creation succeeds.
     *    (settings_mesh_state_t etc.) are heap-allocated to stay within budget.
     */
    ESP_LOGI(TAG, "Internal RAM free before ui_gfx: %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    static StaticTask_t ui_task_tcb;
    static StackType_t ui_task_stack[10240]; /* 10K words = 40KB internal stack */
    /* Pin ui_gfx to CPU0 at priority 2 (below mesh/radio at 5 on CPU1).
     * On shared-SPI boards (T-Deck), running the display task on the same
     * core as mesh at equal priority causes SPI bus contention that wedges
     * the SX1262.  CPU0 + low priority matches the proven Bramble
     * architecture: radio always preempts display. */
    TaskHandle_t ui_task_handle = xTaskCreateStaticPinnedToCore(
        ui_graphics_task,
        "ui_gfx",
        10240,
        NULL,
        2,
        ui_task_stack,
        &ui_task_tcb,
        0);
    if (ui_task_handle == NULL) {
        ESP_LOGE(TAG, "FAILED to create ui_gfx task (static alloc). Display will be blank.");
    } else {
        ESP_LOGI(TAG, "ui_gfx task created (10K words static stack in internal RAM)");
    }
#else
    /* Init text UI state machine (Heltec and other non-graphical boards) */
    ESP_LOGI(TAG, "=== BOOT STAGE: ui_init ===");
    ui_state_t ui;
    ui_init(&ui);
    int last_message_count = msg_store_count();

    /* Render initial screen */
    ESP_LOGI(TAG, "=== BOOT STAGE: initial render ===");
    render_screen(&ui);
#endif

    ESP_LOGI(TAG, "=== BOOT STAGE: main loop start ===");

    while (1) {
        log_heap_diagnostics_periodic();
#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
        /* LVGL runs in its own task — main loop just keeps watchdog happy */
        poll_connectivity_events();
        vTaskDelay(pdMS_TO_TICKS(1000));
#else
        /* Main loop — 50ms tick (20 Hz) */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        poll_connectivity_events();

        /* Poll button / trackball */
        ui_button_t btn = BTN_NONE;
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        btn = trackball_poll();
        /* keyboard_poll handled separately for text input later */
#else
        btn = button_poll(now_ms);
#endif
        if (btn != BTN_NONE) {
            ESP_LOGI(TAG, "Button event: %d", btn);
            ui_handle_button(&ui, btn, now_ms);
        }

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        /* Keyboard input — only active on compose screen */
        char key;
        while (keyboard_poll(&key)) {
            if (ui_get_screen(&ui) == SCREEN_COMPOSE) {
                if (key == '\n' || key == '\r') {
                    /* Send the message */
                    if (ui.compose_len > 0) {
                        ui.compose_buf[ui.compose_len] = '\0';
                        mesh_send_broadcast((const uint8_t *)ui.compose_buf, ui.compose_len);
                        ui.compose_len = 0;
                        ui.compose_buf[0] = '\0';
                        /* Return to messages screen */
                        ui.current_screen = SCREEN_MESSAGES;
                    }
                } else if (key == '\b' || key == 127) {
                    /* Backspace */
                    if (ui.compose_len > 0) {
                        ui.compose_len--;
                        ui.compose_buf[ui.compose_len] = '\0';
                    }
                } else if (key == 27) {
                    /* Escape — cancel compose */
                    ui.compose_len = 0;
                    ui.compose_buf[0] = '\0';
                    ui.current_screen = SCREEN_MESSAGES;
                } else if (key >= 32 && key < 127 && ui.compose_len < COMPOSE_BUF_SIZE - 1) {
                    /* Regular character */
                    ui.compose_buf[ui.compose_len++] = key;
                    ui.compose_buf[ui.compose_len] = '\0';
                }
                ui.screen_dirty = true;
            }
        }
#endif

        /* Incoming message detection for text UI boards. */
        {
            int current_message_count = msg_store_count();
            if (current_message_count > last_message_count) {
                ui_on_message_received(&ui, now_ms);
            }
            last_message_count = current_message_count;
        }

        /* Handle settings confirmation */
        if (ui.settings_confirmed) {
            ui.settings_confirmed = false;
            ui.settings_editing = false;
            if (ui.settings_item_cursor == UI_SETTINGS_ITEM_CONN_MODE) {
                conn_mode_t new_mode = (conn_mode_t)ui.settings_cursor;
                conn_mode_t old_mode = conn_mode_get();
                if (new_mode != old_mode) {
                    conn_mode_set(new_mode);
                    ESP_LOGI(TAG, "Connectivity mode changed to %d, rebooting...", new_mode);

                    /* Show confirmation before reboot */
                    display_clear();
                    static const char *mnames[] = {"WiFi", "BLE"};
                    
                    const char *msg1 = "Mode changed:";
                    int msg1_x = (DISPLAY_WIDTH - strlen(msg1) * FONT_W) / 2;
                    int msg1_y = DISPLAY_HEIGHT / 4;
                    display_draw_text(msg1_x, msg1_y, msg1);
                    
                    int mode_w = strlen(mnames[new_mode]) * FONT_W * 2;
                    int mode_x = (DISPLAY_WIDTH - mode_w) / 2;
                    int mode_y = msg1_y + FONT_H + 8;
                    display_draw_text_large(mode_x, mode_y, mnames[new_mode]);
                    
                    const char *msg2 = "Rebooting...";
                    int msg2_x = (DISPLAY_WIDTH - strlen(msg2) * FONT_W) / 2;
                    int msg2_y = mode_y + LARGE_FONT_H + 8;
                    display_draw_text(msg2_x, msg2_y, msg2);
                    
                    display_flush();
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    esp_restart();
                } else {
                    ui.screen_dirty = true;
                }
            } else if (ui.settings_item_cursor == UI_SETTINGS_ITEM_LOCATION) {
                loc_share_mode_t new_loc = (loc_share_mode_t)ui.settings_cursor;
                loc_share_mode_t old_loc = location_share_mode_get();
                if (new_loc != old_loc) {
                    location_share_mode_set(new_loc);
                    static const char *loc_names[] = {"Off", "Coarse", "Exact"};
                    ESP_LOGI(TAG, "Location sharing set to %s", loc_names[new_loc]);
                }
                ui.screen_dirty = true;
            } else {
                /* OLED rotation */
                bool new_rot = (ui.settings_cursor == 1);
                bool old_rot = display_get_rotated_180();
                if (new_rot != old_rot) {
                    display_set_rotated_180(new_rot);
                    /* Persist to NVS */
                    nvs_handle_t nvs;
                    if (nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &nvs) == ESP_OK) {
                        nvs_set_u8(nvs, NVS_KEY_OLED_ROT, new_rot ? 1 : 0);
                        nvs_commit(nvs);
                        nvs_close(nvs);
                    }
                    ESP_LOGI(TAG, "OLED rotation set to %s", new_rot ? "180 deg" : "Normal");
                }
                ui.screen_dirty = true;
            }
        }

        /* Check inactivity timeout */
        ui_check_timeout(&ui, now_ms);

        /* Redraw if needed */
        if (ui_needs_redraw(&ui)) {
            render_screen(&ui);
            ui_mark_drawn(&ui);
        }

        /* Periodic refresh of main screen (uptime counter) */
        if (ui_get_screen(&ui) == SCREEN_MAIN && (now_ms % 1000) < 50) {
            render_main_screen();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
#endif
    }
}
