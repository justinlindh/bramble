#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs_keys.h"
#include "ota_rollback.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "display.h"
#include "button.h"
#include "ui.h"
#include "crypto.h"
#include "crypto_entropy.h"
#ifndef CONFIG_IDF_TARGET_LINUX
#include "bootloader_random.h"
#endif
#include "secure_nvs.h"
#include "esp_partition.h"
#include "identity.h"
#include "mesh_task.h"
#include "cli.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include "wifi_manager.h"
#include "ws_server.h"
#include "msg_store.h"
#include "esp_spiffs.h"
/* mdns is excluded on the POSIX/Linux simulator (see idf_component.yml);
 * it is only reachable from the WiFi-connected path, which the simulator's
 * wifi_manager stub never takes. */
#ifndef CONFIG_IDF_TARGET_LINUX
#include "mdns.h"
#endif
#include "ble_server.h"
#if CONFIG_BT_ENABLED
/* esp_bt_mem_release: reclaim the unused BT controller RAM on WiFi boots. */
#include "esp_bt.h"
#endif
#include "esp_system.h"
#include "battery.h"
#include "alerts.h"
#include "indicators.h"
#include "board_config.h"
#include "keyboard.h"
#include "trackball.h"
#include "location.h"
#include "sas_format.h"

#include "gps.h"
#include "gnss_status.h"
#include "gps_pref.h"
#include "cJSON.h"

#ifdef CONFIG_IDF_TARGET_LINUX
/* Emulator (IDF linux target) only: the emu-link broker client and the
 * per-node NVS-flash persistence hook. Both are host-only; the device build
 * compiles neither this include nor the calls gated on it below. */
#include "emu_link.h"
/* Provided by the emulator node's null_drivers component (emu_flash_persist.c). */
void emu_node_flash_persist_init(void);
/* Scripted-send hook (emu_autosend.c): originates a message on cue in a
 * scenario. No-op unless EMU_AUTO_SEND is set. */
int emu_node_start_autosend(void);
#endif

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#include "sdcard.h"
#include "audio.h"
#endif

#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
/* lvgl.h not directly included, use ui_graphics API */
#include "ui_graphics.h"
#endif

static const char* TAG = "bramble";

/* ── Layout constants derived from display size ─────────────────────── */

#define FONT_W 6
#define FONT_H 8
#define LINE_H (FONT_H + 2)       /* 10px per line */
#define LARGE_FONT_H (FONT_H * 2) /* 16px */
#define HEADER_Y 0
#define DIVIDER_Y (FONT_H + 2)    /* below header text */
#define CONTENT_Y (DIVIDER_Y + 4) /* below divider */
#define FOOTER_Y (DISPLAY_HEIGHT - FONT_H - 2)
#define CHARS_PER_LINE (DISPLAY_WIDTH / FONT_W)

/* ── Location manager ───────────────────────────────────────────────── */

static bool s_last_gps_fix = false;
static bool s_has_last_wifi_status = false;
static wifi_status_t s_last_wifi_status = {0};

static void emit_gps_event(const char* event, const bramble_position_t* pos) {
    cJSON* params = cJSON_CreateObject();
    if (!params)
        return;
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

static void emit_wifi_event(const wifi_status_t* st, const char* event) {
    cJSON* params = cJSON_CreateObject();
    if (!params)
        return;
    const bool connected = st->ip_addr[0] != '\0';
    const char* mode = "off";
    if (st->mode == BRAMBLE_WIFI_STATION)
        mode = "sta";
    else if (st->mode == BRAMBLE_WIFI_AP)
        mode = "ap";

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
    } else if (strncmp(st.ip_addr, s_last_wifi_status.ip_addr, sizeof(st.ip_addr)) != 0 &&
               connected) {
        emit_wifi_event(&st, "ip_changed");
    }
    s_last_wifi_status = st;
}

static void log_heap_diagnostics_periodic(void) {
    static uint64_t s_last_log_us = 0;
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (s_last_log_us != 0 && (now_us - s_last_log_us) < 30000000ULL) {
        return;
    }
    s_last_log_us = now_us;

    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t min_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(
        TAG,
        "diag.heap internal_free=%u internal_min=%u internal_largest=%u psram_free=%u psram_min=%u",
        (unsigned)free_internal, (unsigned)min_internal, (unsigned)largest_internal,
        (unsigned)free_psram, (unsigned)min_psram);
}

static void on_gps_fix(const bramble_position_t* pos, void* ctx) {
    /* No position bookkeeping here: this callback used to copy every fix into a
     * local location_manager_t that nothing ever read, while the manager the UI
     * reads (mesh_task's) starved. Consumers now pull self-position straight
     * from gps_get_position via mesh_resolve_self_position; this callback only
     * handles event emission. */
    (void)ctx;
    if (pos && pos->valid) {
        s_last_gps_fix = true;
        /* Throttle RPC notifications + log to avoid ~60/min of GPS chatter. */
        static uint64_t s_last_gps_notify_us = 0;
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (s_last_gps_notify_us == 0 || (now_us - s_last_gps_notify_us) >= 5000000ULL) {
            s_last_gps_notify_us = now_us;
            emit_gps_event("fix_acquired", pos);
            ESP_LOGI(TAG, "GPS position updated: lat=%.6f lon=%.6f alt=%d", pos->latitude_e7 / 1e7,
                     pos->longitude_e7 / 1e7, pos->altitude_m);
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
    /* Explicit allowlist: the enum is sparse (2 is the retired BOTH slot,
     * still normalized to WiFi so stale devices do not silently go dark)
     * and Off sits above it at 3, so a range check cannot work. */
    if (mode != CONN_MODE_WIFI && mode != CONN_MODE_BLE && mode != CONN_MODE_OFF)
        mode = CONN_MODE_WIFI;

    return conn_mode_resolve_boot((conn_mode_t)mode, ble_server_supported());
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

/* ── GPS enable (NVS-persisted) ─────────────────────────────────────── */

/* The persisted GPS power preference itself now lives in the shared
 * components/gps/gps_pref.c (gps_pref_get()/gps_pref_set()), so the nRF
 * target and the RPC layer can reach it too; this file just delegates. */

/* Read-only view of the persisted GPS power preference, so the T-Deck status
 * bar can dim its GPS icon when GPS is switched off in Settings. Self-declared
 * to satisfy -Wmissing-prototypes for this cross-module accessor. */
bool bramble_gps_enabled(void);
bool bramble_gps_enabled(void) { return gps_pref_get(); }

/* ── Splash screen ──────────────────────────────────────────────────── */

#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
/**
 * Show a boot-progress status line on the OLED during initialization.
 * Redraws the BRAMBLE header + divider and places msg below the divider.
 * Only used on non-graphical boards (Heltec V3 SSD1306 OLED).
 */
static void show_boot_status(const char* msg) {
    display_clear();

    /* Redraw BRAMBLE header (same layout as show_splash) */
    int title_w = 7 * FONT_W * 2;
    int title_x = (DISPLAY_WIDTH - title_w) / 2;
    int title_y = DISPLAY_HEIGHT / 4;
    display_draw_text_large(title_x, title_y, "BRAMBLE");

    int div_y = title_y + LARGE_FONT_H + 4;
    display_hline(DISPLAY_WIDTH / 8, div_y, DISPLAY_WIDTH * 3 / 4);

    /* Status line below divider, truncate to display width */
    char buf[22]; /* 128px / 6px-per-char + NUL */
    snprintf(buf, sizeof(buf), "%s", msg);
    display_draw_text(2, div_y + 8, buf);

    display_flush();
}
#endif /* CONFIG_BRAMBLE_UI_GRAPHICAL */

static void show_splash(void) {
    display_clear();

    /* "BRAMBLE" in large text, centered */
    int title_w = 7 * FONT_W * 2; /* 7 chars × 12px */
    int title_x = (DISPLAY_WIDTH - title_w) / 2;
    int title_y = DISPLAY_HEIGHT / 4;
    display_draw_text_large(title_x, title_y, "BRAMBLE");

    /* Divider line */
    int div_y = title_y + LARGE_FONT_H + 4;
    display_hline(DISPLAY_WIDTH / 8, div_y, DISPLAY_WIDTH * 3 / 4);

    /* Tagline centered */
    const char* tag = "LoRa Mesh Network";
    int tag_w = strlen(tag) * FONT_W;
    display_draw_text((DISPLAY_WIDTH - tag_w) / 2, div_y + 8, tag);

    /* Version centered */
    const char* ver = esp_app_get_description()->version;
    int ver_w = strlen(ver) * FONT_W;
    display_draw_text((DISPLAY_WIDTH - ver_w) / 2, div_y + 20, ver);

    display_flush();
}

/* ── Screen renderers ───────────────────────────────────────────────── */

static bramble_identity_t g_identity;
static uint32_t my_addr = 0;
static uint32_t boot_time_ms = 0;

/* Shared render-time snapshots: only one screen renders at a time,
 * so a single static instance avoids ~8KB of duplicate BSS. */
static mesh_shared_state_t s_render_mesh;
static routing_table_t s_render_routes;

/* Nodes screen: the addr of the peer currently shown on the SAS detail
 * sub-screen, set by render_screen() so the main loop's node_verify_confirmed
 * consumer verifies exactly the peer the user is looking at, immune to list
 * reordering between the button press and the render. */
static uint32_t s_nodes_detail_addr;
static bool s_nodes_detail_valid;

/* Nodes screen "selectable neighbors" = entries with addr != 0, in array
 * order. node_total, the cursor highlight, and the detail addr all derive
 * from this pair so they can never disagree about which peer is selected. */
static int selectable_neighbor_count(const neighbor_table_t* neighbors) {
    int n = 0;
    for (int i = 0; i < neighbors->count; i++) {
        if (neighbors->entries[i].addr != 0)
            n++;
    }
    return n;
}

static uint32_t selectable_neighbor_addr(const neighbor_table_t* neighbors, int k) {
    int seen = 0;
    for (int i = 0; i < neighbors->count; i++) {
        if (neighbors->entries[i].addr == 0)
            continue;
        if (seen == k)
            return neighbors->entries[i].addr;
        seen++;
    }
    return 0;
}

/* Per-neighbor SAS-verification glyph for the Nodes list: " *" once verified,
 * " !" when the identity key changed since the last verify (re-verify
 * needed), "" otherwise (no pin yet, or pinned-but-unverified). */
/* Centered "(no neighbors yet)" placeholder for the Nodes list, shared by
 * the cursor-selection and plain-browse render branches (both show it in
 * the same empty-list layout). */
static void render_no_neighbors(int y) {
    int no_nbr_y = y + ((FOOTER_Y - y - FONT_H) / 2);
    if (no_nbr_y < y)
        no_nbr_y = y;
    const char* no_nbr = "(no neighbors yet)";
    int no_nbr_x = (DISPLAY_WIDTH - strlen(no_nbr) * FONT_W) / 2;
    display_draw_text(no_nbr_x, no_nbr_y, no_nbr);
}

static const char* node_verify_glyph(uint32_t addr) {
    bool verified = false;
    bool key_changed = false;
    if (!mesh_get_peer_verify_flags(addr, &verified, &key_changed))
        return "";
    if (key_changed)
        return " !";
    if (verified)
        return " *";
    return "";
}

/* "*N" unread badge, drawn right-aligned at the given x limit on every
 * screen except Messages itself. */
static void render_unread_badge(const ui_state_t* ui, int right_x) {
    if (ui->unread_count <= 0 || ui->current_screen == SCREEN_MESSAGES)
        return;
    /* Sized for a full int, so -Wformat-truncation holds on every target. */
    char b[16];
    if (ui->unread_count > 9)
        snprintf(b, sizeof(b), "*9+");
    else
        snprintf(b, sizeof(b), "*%d", ui->unread_count);
    display_draw_text(right_x - (int)strlen(b) * FONT_W, HEADER_Y, b);
}

static void render_main_screen(const ui_state_t* ui) {
    display_clear();

    /* Header: name + battery, right-aligned battery */
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
        render_unread_badge(ui, batt_x - FONT_W);
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

    /* WiFi IP address (if connected), else last RX signal.
     * In AP mode the IP is the constant 192.168.4.1, which tells the user
     * nothing they cannot guess, so that line carries the per-device AP
     * password instead: on a screen-only board this is how the operator
     * learns it (issue #78). Line budget is unchanged either way. */
    wifi_status_t wst;
    wifi_manager_get_status(&wst);
    const char* ip = wifi_manager_get_ip();
    if (wst.mode == BRAMBLE_WIFI_AP && wst.ap_password[0] != '\0') {
        /* Bounded precision: a derived password is exactly
         * WIFI_AP_PASSWORD_LEN, and a longer fixed override would not fit a
         * 21-character OLED line anyway (the serial CLI prints it in full). */
        snprintf(line, sizeof(line), "AP PW: %.*s", WIFI_AP_PASSWORD_LEN, wst.ap_password);
    } else if (ip && ip[0] != '\0') {
        snprintf(line, sizeof(line), "IP: %s", ip);
    } else if (n > 0) {
        snprintf(line, sizeof(line), "RSSI:%d SNR:%d", s_render_mesh.last_rx_rssi,
                 s_render_mesh.last_rx_snr);
    } else {
        line[0] = '\0';
    }
    if (line[0]) {
        display_draw_text(2, y, line);
    }
    y += LINE_H;

    /* Uptime */
    uint32_t up_sec = (uint32_t)((esp_timer_get_time() / 1000000ULL) - (boot_time_ms / 1000));
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
            const route_entry_t* r = &s_render_routes.entries[i];
            if (r->state == ROUTE_STALE || r->state == ROUTE_BROKEN)
                continue;
            bool is_direct = false;
            for (int j = 0; j < s_render_mesh.neighbors.count; j++) {
                if (s_render_mesh.neighbors.entries[j].addr == r->dest_addr) {
                    is_direct = true;
                    break;
                }
            }
            if (!is_direct)
                routed++;
        }
        int total = direct + routed;
        snprintf(line, sizeof(line), "Reach:%d (%d direct)", total, direct);
    }
    display_draw_text(2, y, line);

    display_flush();
}

static void render_screen(ui_state_t* ui) {
    /* E-paper ghosting cleanup (bramble#196): the screen ring's full-refresh
     * policy (ui.h, UI_FULL_REFRESH_EVERY_N_SCREENS) decides WHEN to clear
     * accumulated ghosting; this is the one place that acts on it, right
     * before the frame that policy targeted gets drawn. No-op on OLED/LCD
     * boards (display_request_full_refresh() is a stub there). */
    if (ui_take_full_refresh_pending(ui))
        display_request_full_refresh();

    switch (ui_get_screen(ui)) {
    case SCREEN_MAIN:
        render_main_screen(ui);
        break;
    case SCREEN_MESSAGES: {
        display_clear();
        int mcount = msg_store_count();
        char hdr[32];
        if (ui->msg_scroll > 0)
            snprintf(hdr, sizeof(hdr), "Messages (%d) ^%d", mcount, ui->msg_scroll);
        else
            snprintf(hdr, sizeof(hdr), "Messages (%d)", mcount);
        display_draw_text(2, HEADER_Y, hdr);
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);

        if (mcount == 0) {
            int no_msg_y = (DISPLAY_HEIGHT - FONT_H) / 2;
            const char* no_msg = "(no messages yet)";
            int no_msg_x = (DISPLAY_WIDTH - strlen(no_msg) * FONT_W) / 2;
            display_draw_text(no_msg_x, no_msg_y, no_msg);
        } else {
            mesh_get_state(&s_render_mesh); /* for beacon names */
            uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            int max_msgs = (FOOTER_Y - CONTENT_Y) / LINE_H;
            int start = mcount - max_msgs - ui->msg_scroll;
            if (start < 0)
                start = 0;
            int y = CONTENT_Y;
            for (int i = start; i < mcount && y < FOOTER_Y; i++) {
                const stored_msg_t* m = msg_store_get(i);
                if (!m)
                    continue;

                bool outgoing =
                    (m->direction == MSG_DIR_OUTGOING || m->direction == MSG_DIR_BROADCAST_OUT);

                /* Delivery badge for outgoing messages (2 chars at end) */
                const char* badge = "";
                if (m->direction == MSG_DIR_OUTGOING) {
                    switch (m->status) {
                    case MSG_STATUS_SENT:
                        badge = " *";
                        break; /* pending */
                    case MSG_STATUS_DELIVERED:
                        badge = (m->route_hop_count > 1) ? "++" : " +";
                        break; /* acked */
                    case MSG_STATUS_FAILED:
                        badge = " x";
                        break; /* failed */
                    default:
                        break;
                    }
                }

                const char* peer_name = NULL;
                if (!outgoing) {
                    neighbor_entry_t* nb = neighbor_lookup(&s_render_mesh.neighbors, m->peer_addr);
                    if (nb && nb->name[0])
                        peer_name = nb->name;
                }

                ui_msg_line_t li = {
                    .text = m->text,
                    .text_len = m->text_len,
                    .outgoing = outgoing,
                    .peer_addr = m->peer_addr,
                    .peer_name = peer_name,
                    .channel_index = m->channel_index,
                    .channel_name =
                        (m->channel_index > 0) ? mesh_get_channel_name(m->channel_index) : NULL,
                    .badge = badge,
                    .age_s = (m->timestamp_s == 0)
                                 ? -1
                                 : ((now_s >= m->timestamp_s) ? (int)(now_s - m->timestamp_s) : 0),
                };
                char line[CHARS_PER_LINE + 1];
                ui_format_msg_line(&li, line, sizeof(line));
                display_draw_text(2, y, line);
                y += LINE_H;
            }
        }
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        display_draw_text(2, FOOTER_Y, "[o] compose  < > navigate");
#else
        if (ui->msg_scroll > 0)
            display_draw_text(2, FOOTER_Y, "[2x]new [hold]older");
        else
            display_draw_text(2, FOOTER_Y, "[hold]older reply:app");
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
            const route_entry_t* r = &s_render_routes.entries[i];
            if (r->dest_addr == 0)
                continue;
            if (r->state == ROUTE_STALE || r->state == ROUTE_BROKEN)
                continue;
            route_count++;
            hop_total += r->hop_count;
        }
        int avg_hops_tenths = (route_count > 0) ? ((hop_total * 10) / route_count) : 0;

        char hdr[32];
        snprintf(hdr, sizeof(hdr), "Nodes (%d routes)", route_count);
        display_draw_text(2, HEADER_Y, hdr);
        render_unread_badge(ui, DISPLAY_WIDTH - 2);
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);

        char summary[32];
        snprintf(summary, sizeof(summary), "Avg: %d.%d hops", avg_hops_tenths / 10,
                 avg_hops_tenths % 10);
        int y = CONTENT_Y;
        display_draw_text(2, y, summary);
        y += LINE_H;

        int cnt = neighbor_count(&s_render_mesh.neighbors);
        if (ui->node_detail_open) {
            /* Per-peer SAS detail sub-screen. */
            int k = ui->nodes_cursor;
            uint32_t addr = selectable_neighbor_addr(&s_render_mesh.neighbors, k);
            if (addr == 0) {
                s_nodes_detail_valid = false;
                display_draw_text(2, y, "no peer");
            } else {
                s_nodes_detail_addr = addr;
                s_nodes_detail_valid = true;

                neighbor_entry_t* nb = neighbor_lookup(&s_render_mesh.neighbors, addr);
                char nl[40];
                if (nb && nb->name[0])
                    snprintf(nl, sizeof(nl), "%.24s", nb->name);
                else
                    snprintf(nl, sizeof(nl), "%08" PRIX32, addr);
                display_draw_text(2, y, nl);
                y += LINE_H + 4;

                char sas[8];
                bool verified = false;
                bool key_changed = false;
                if (!mesh_get_peer_verification(addr, sas, &verified, &key_changed)) {
                    display_draw_text(2, y, "No secure session yet");
                } else {
                    char grouped[9];
                    sas_format_grouped(sas, grouped);
                    int sas_x = (DISPLAY_WIDTH - (int)strlen(grouped) * FONT_W * 2) / 2;
                    if (sas_x < 2)
                        sas_x = 2;
                    display_draw_text_large(sas_x, y, grouped);
                    y += LARGE_FONT_H + 4;

                    if (key_changed)
                        display_draw_text(2, y, "KEY CHANGED - re-verify");
                    else if (verified)
                        display_draw_text(2, y, "VERIFIED");
                    else
                        display_draw_text(2, y, "unverified");
                    y += LINE_H;

                    if (ui->node_verify_armed)
                        display_draw_text(2, y, "[hold again] confirm");
                    else
                        display_draw_text(2, y, "[hold] verify");
                }
            }
            display_draw_text(2, FOOTER_Y, "[2x] back");
        } else if (ui->nodes_selecting) {
            /* Cursor-selection list: highlight the k-th selectable neighbor. */
            s_nodes_detail_valid = false;
            if (cnt == 0) {
                render_no_neighbors(y);
            } else {
                char nl[64];
                int k = 0;
                for (int i = 0; i < s_render_mesh.neighbors.count && y < FOOTER_Y; i++) {
                    neighbor_entry_t* e = &s_render_mesh.neighbors.entries[i];
                    if (e->addr == 0)
                        continue;
                    const char* cursor = (k == ui->nodes_cursor) ? ">" : " ";
                    const char* glyph = node_verify_glyph(e->addr);
                    if (e->name[0])
                        snprintf(nl, sizeof(nl), "%s%.8s%s", cursor, e->name, glyph);
                    else
                        snprintf(nl, sizeof(nl), "%s%08" PRIX32 "%s", cursor, e->addr, glyph);
                    display_draw_text(2, y, nl);
                    y += LINE_H;
                    k++;
                }
            }
            display_draw_text(2, FOOTER_Y, "[short]next [hold]open [2x]back");
        } else {
            s_nodes_detail_valid = false;
            if (cnt == 0) {
                render_no_neighbors(y);
            } else {
                char nl[64];
                uint32_t now_ms_n = (uint32_t)(esp_timer_get_time() / 1000ULL);
                for (int i = 0; i < s_render_mesh.neighbors.count && y < FOOTER_Y; i++) {
                    neighbor_entry_t* e = &s_render_mesh.neighbors.entries[i];
                    if (e->addr == 0)
                        continue;
                    /* Format last-seen age: seconds, minutes, or hours */
                    char age_str[8];
                    uint32_t age_ms = (e->last_heard > 0 && now_ms_n >= e->last_heard)
                                          ? (now_ms_n - e->last_heard)
                                          : 0;
                    uint32_t age_s = age_ms / 1000;
                    if (age_s < 60) {
                        snprintf(age_str, sizeof(age_str), "%lus", (unsigned long)age_s);
                    } else if (age_s < 3600) {
                        snprintf(age_str, sizeof(age_str), "%lum", (unsigned long)(age_s / 60));
                    } else {
                        snprintf(age_str, sizeof(age_str), "%luh+", (unsigned long)(age_s / 3600));
                    }
                    const char* glyph = node_verify_glyph(e->addr);
                    /* Prefer the beacon name; fall back to the full address. */
                    if (e->name[0])
                        snprintf(nl, sizeof(nl), "%.8s %d %s%s", e->name, e->rssi, age_str, glyph);
                    else
                        snprintf(nl, sizeof(nl), "%08" PRIX32 " %d %s%s", e->addr, e->rssi, age_str,
                                 glyph);
                    display_draw_text(2, y, nl);
                    y += LINE_H;
                }
            }
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
            display_draw_text(2, FOOTER_Y, "< > navigate  [o] select");
#else
            display_draw_text(2, FOOTER_Y, "[hold] verify contacts");
#endif
        }
        display_flush();
        break;
    }
    case SCREEN_SETTINGS: {
        display_clear();
        display_draw_text(2, HEADER_Y, "Settings");
        render_unread_badge(ui, DISPLAY_WIDTH - 2);
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);

        if (ui->settings_editing) {
            int y = CONTENT_Y;
            if (ui->settings_item_cursor == UI_SETTINGS_ITEM_CONN_MODE) {
                display_draw_text(2, y, "Connectivity Mode:");
                y += LINE_H + 4;
                conn_mode_t current = conn_mode_get();
                for (int i = 0; i < CONN_MODE_UI_COUNT; i++) {
                    char ml[32];
                    conn_mode_t m = conn_mode_from_ui_index(i);
                    const char* arrow = (i == ui->settings_cursor) ? ">" : " ";
                    const char* mark = (m == current) ? " *" : "";
                    snprintf(ml, sizeof(ml), "%s %s%s", arrow, conn_mode_name(m), mark);
                    display_draw_text(2, y, ml);
                    y += LINE_H;
                }
            } else if (ui->settings_item_cursor == UI_SETTINGS_ITEM_LOCATION) {
                display_draw_text(2, y, "Location Sharing:");
                y += LINE_H + 4;
                static const char* loc_names[] = {"Off", "Coarse", "Exact"};
                loc_share_mode_t cur_loc = location_share_mode_get();
                for (int i = 0; i < LOC_SHARE_COUNT; i++) {
                    char ml[32];
                    const char* arrow = (i == ui->settings_cursor) ? ">" : " ";
                    const char* mark = (i == (int)cur_loc) ? " *" : "";
                    snprintf(ml, sizeof(ml), "%s %s%s", arrow, loc_names[i], mark);
                    display_draw_text(2, y, ml);
                    y += LINE_H;
                }
            } else if (ui->settings_item_cursor == UI_SETTINGS_ITEM_GPS) {
                display_draw_text(2, y, "GPS:");
                y += LINE_H + 4;
                static const char* gps_names[] = {"Off", "On"};
                bool cur_gps = gps_pref_get();
                for (int i = 0; i < 2; i++) {
                    char ml[32];
                    const char* arrow = (i == ui->settings_cursor) ? ">" : " ";
                    const char* mark = (i == (cur_gps ? 1 : 0)) ? " *" : "";
                    snprintf(ml, sizeof(ml), "%s %s%s", arrow, gps_names[i], mark);
                    display_draw_text(2, y, ml);
                    y += LINE_H;
                }
            } else {
                /* OLED rotation */
                display_draw_text(2, y, "OLED Rotation:");
                y += LINE_H + 4;
                static const char* rot_names[] = {"Normal", "180 deg"};
                bool cur_rot = display_get_rotated_180();
                for (int i = 0; i < 2; i++) {
                    char ml[32];
                    const char* arrow = (i == ui->settings_cursor) ? ">" : " ";
                    const char* mark = (i == (int)cur_rot) ? " *" : "";
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
            loc_share_mode_t cur_loc = location_share_mode_get();
            static const char* loc_names[] = {"Off", "Coarse", "Exact"};

            /* Row 0: Connectivity */
            {
                const char* sel =
                    (ui->settings_item_cursor == UI_SETTINGS_ITEM_CONN_MODE) ? ">" : " ";
                snprintf(line, sizeof(line), "%sConn: %s", sel, conn_mode_name(cur_mode));
                display_draw_text(2, y, line);
                y += LINE_H;
            }
            /* Row 1: OLED Rotation (placeholder) */
            {
                const char* sel =
                    (ui->settings_item_cursor == UI_SETTINGS_ITEM_OLED_ROTATION) ? ">" : " ";
                snprintf(line, sizeof(line), "%sRotation: 0", sel);
                display_draw_text(2, y, line);
                y += LINE_H;
            }
            /* Row 2: Location Sharing */
            {
                const char* sel =
                    (ui->settings_item_cursor == UI_SETTINGS_ITEM_LOCATION) ? ">" : " ";
                snprintf(line, sizeof(line), "%sLocation: %s", sel, loc_names[cur_loc]);
                display_draw_text(2, y, line);
                y += LINE_H;
            }
            /* Row 3: GPS power (only on GPS boards) */
            if (ui->gps_available) {
                const char* sel = (ui->settings_item_cursor == UI_SETTINGS_ITEM_GPS) ? ">" : " ";
                snprintf(line, sizeof(line), "%sGPS: %s", sel, gps_pref_get() ? "On" : "Off");
                display_draw_text(2, y, line);
                y += LINE_H;
            }
            /* Device info row, drawn only when it clears the footer. On the
             * 128x64 OLED the four setting rows push this line down to
             * FOOTER_Y once the GPS row is present, so it would overlap the
             * footer hint; the taller e-paper always has room. The address is
             * also shown on the main screen. */
            if (y + FONT_H <= FOOTER_Y) {
                snprintf(line, sizeof(line), "Addr: %08" PRIX32, my_addr);
                display_draw_text(2, y, line);
            }
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
            display_draw_text(2, FOOTER_Y, "[o] edit  < > navigate");
#else
            display_draw_text(2, FOOTER_Y, "[hold]edit [2x]exit");
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
        int chars_per_row = CHARS_PER_LINE - 1; /* leave margin */

        /* Word-wrap compose buffer */
        for (int i = 0; i < ui->compose_len && y < FOOTER_Y - LINE_H;) {
            char row[64];
            int row_len =
                (ui->compose_len - i > chars_per_row) ? chars_per_row : ui->compose_len - i;
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
        render_unread_badge(ui, DISPLAY_WIDTH - 2);
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);

        char line[64];
        int y = CONTENT_Y;

        /* Line 1: TX/RX counters */
        mesh_get_state(&s_render_mesh);
        snprintf(line, sizeof(line), "TX:%" PRIu32 " RX:%" PRIu32, s_render_mesh.packets_tx,
                 s_render_mesh.packets_rx);
        display_draw_text(2, y, line);
        y += LINE_H;

        /* Line 2: Airtime TX budget used % (NORMAL tier) */
        {
            uint32_t used_ms = 0;
            uint32_t max_ms = s_render_mesh.airtime.max_ms[0];
            if (max_ms > 0 && s_render_mesh.airtime.tokens_ms[0] <= max_ms) {
                used_ms = max_ms - s_render_mesh.airtime.tokens_ms[0];
            }
            uint32_t pct_tenths = (max_ms > 0) ? (used_ms * 1000 / max_ms) : 0;
            snprintf(line, sizeof(line), "Air: %" PRIu32 ".%" PRIu32 "%% TX", pct_tenths / 10,
                     pct_tenths % 10);
        }
        display_draw_text(2, y, line);
        y += LINE_H;

        /* Line 3: Heap + Uptime */
        {
            uint32_t heap_kb = esp_get_free_heap_size() / 1024;
            uint32_t up_sec =
                (uint32_t)((esp_timer_get_time() / 1000000ULL) - (boot_time_ms / 1000));
            char uptime[20];
            ui_format_uptime(up_sec, uptime, sizeof(uptime));
            snprintf(line, sizeof(line), "Heap:%" PRIu32 "KB Up:%s", heap_kb, uptime);
        }
        display_draw_text(2, y, line);
        y += LINE_H;

        /* Line 4: Routes + Battery */
        {
            mesh_get_routes(&s_render_routes);
            int route_cnt = 0;
            for (int i = 0; i < s_render_routes.count; i++) {
                route_state_t st = s_render_routes.entries[i].state;
                if (st != ROUTE_STALE && st != ROUTE_BROKEN)
                    route_cnt++;
            }
            uint8_t bpct = battery_read_pct();
            snprintf(line, sizeof(line), "Routes:%d Batt:%u%%", route_cnt, (unsigned)bpct);
        }
        display_draw_text(2, y, line);

        /* Delivery badge legend for the messages screen */
        display_draw_text(2, FOOTER_Y, "*pend +ok ++mh x fail");
#endif

        display_flush();
        break;
    }
    case SCREEN_GPS: {
        /* Only reachable when board_has_cap(BOARD_CAP_GPS) - see ui_set_gps_available() call
         * in app_main and the gating in ui_handle_button(). This layout serves the
         * non-graphical boards; the graphical stack renders GNSS through scr_layout's
         * status bar and scr_stats, from the same gnss_ui_classify() verdict. */
        display_clear();
        display_draw_text(2, HEADER_Y, "GPS");
        render_unread_badge(ui, DISPLAY_WIDTH - 2);
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);

        int y = CONTENT_Y;
        char line[32];

        gps_stats_t stats = {0};
        gps_get_stats(&stats);
        bool has_fix = gps_has_fix();

        /* Line 1: the three-way state, so "no fix" is separated into a receiver
         * hearing nothing and a receiver hearing satellites and converging. */
        gnss_ui_input_t gnss = {
            .board_has_gnss = true,
            .powered = bramble_gps_enabled(),
            .has_fix = has_fix,
            .sats_in_view = stats.sats_in_view,
            .sats_tracked = stats.sats_tracked,
            .sats_used = stats.sats_used,
            .snr_max_dbhz = stats.snr_max_dbhz,
            .fix_quality = stats.fix_quality,
            .nmea_age_s = stats.nmea_age_s,
        };
        snprintf(line, sizeof(line), "GPS: %s", gnss_ui_state_label(gnss_ui_classify(&gnss)));
        display_draw_text(2, y, line);
        y += LINE_H;

        /* Line 2: satellites used / tracked / in view. Tracked is the middle
         * number because a nonzero in-view with zero tracked is the almanac
         * predicting satellites the receiver cannot hear. */
        snprintf(line, sizeof(line), "Sats: %u/%u/%u", (unsigned)stats.sats_used,
                 (unsigned)stats.sats_tracked, (unsigned)stats.sats_in_view);
        display_draw_text(2, y, line);
        y += LINE_H;

        bramble_position_t pos;
        if (has_fix && gps_get_position(&pos)) {
            /* Line 3: lat,lon to 5 decimal places */
            snprintf(line, sizeof(line), "%.5f,%.5f", pos.latitude_e7 / 1e7,
                     pos.longitude_e7 / 1e7);
            display_draw_text(2, y, line);
            y += LINE_H;

            /* Line 4: alt/accuracy. The antenna-open report is suppressed
             * while a fix is valid: passive antennas (e.g. the L76K's
             * onboard ceramic patch) draw no bias current and trip the
             * supervisor permanently even though reception is fine, so with
             * a fix in hand the warning is noise, not signal. */
            snprintf(line, sizeof(line), "Alt:%dm Acc:%um", pos.altitude_m,
                     (unsigned)pos.accuracy_m);
            display_draw_text(2, y, line);
        } else if (stats.antenna_warning) {
            display_draw_text(2, y, "ANTENNA OPEN!");
        }

        display_draw_text(2, FOOTER_Y, "[press] next screen");
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
static void lv_tick_cb(void* arg) {
    (void)arg;
    ui_graphics_tick_1ms();
}

static void ui_graphics_task(void* arg) {
    (void)arg;
    while (1) {
        uint32_t delay = ui_graphics_tick();
        if (delay < 5)
            delay = 5;
        if (delay > 30)
            delay = 30;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}
#endif

void app_main(void) {
    ESP_LOGI(TAG, "=== BOOT STAGE: app_main entry ===");
    ESP_LOGI(TAG, "Bramble LoRa Mesh starting...");

    /* Board-level init: power rails, shared SPI bus */
    ESP_LOGI(TAG, "=== BOOT STAGE: board_init ===");
    if (board_init() != 0) {
        ESP_LOGE(TAG, "Board init failed, halting");
        return;
    }

#ifdef CONFIG_IDF_TARGET_LINUX
    /* Emulator: bind NVS-backed flash to a per-node file ($NODE_DIR/flash.bin)
     * so identity survives a process restart (the supervisor's reset button).
     * Must run before NVS init. A no-op when NODE_DIR is unset (standalone
     * runs keep the ephemeral-temp-flash behavior). */
    emu_node_flash_persist_init();
#endif

    /* NVS init */
    ESP_LOGI(TAG, "=== BOOT STAGE: nvs init ===");
    esp_err_t ret;
#ifdef CONFIG_NVS_ENCRYPTION
    /* SEC-H4: identity/channel/auth-token NVS is encrypted at rest. Keys live
     * in the flash-encryption-protected nvs_keys partition. */
    const esp_partition_t* keys_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);
    nvs_sec_cfg_t sec_cfg;
    nvs_init_action_t plan;
    esp_err_t fail_err = ESP_ERR_NOT_FOUND;
    bool keys_cfg_ok = false;
    bool secure_init_ok = false;
    ret = ESP_OK;
    if (keys_part == NULL) {
        plan = nvs_init_plan(true, false, false, false);
    } else {
        esp_err_t kret = nvs_flash_read_security_cfg(keys_part, &sec_cfg);
        if (kret == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
            kret = nvs_flash_generate_keys(keys_part, &sec_cfg);
        }
        keys_cfg_ok = (kret == ESP_OK);
        fail_err = kret;
        /* Only call secure_init once the keys layer itself is confirmed
         * valid: a corrupt/unreadable keys partition must fail closed, not
         * fall through with a possibly-uninitialized sec_cfg. */
        if (keys_cfg_ok) {
            ret = nvs_flash_secure_init(&sec_cfg);
            secure_init_ok = (ret == ESP_OK);
        }
        plan = nvs_init_plan(true, true, keys_cfg_ok, secure_init_ok);
    }
    switch (plan) {
    case NVS_INIT_FAIL:
        /* Fail closed: a missing keys partition, or any keys-layer
         * read/generate failure (corrupt keys partition, flash error),
         * must abort rather than erase the main NVS partition. Erasing
         * here would wipe the device on a transient fault and re-wipe
         * on every boot if the fault persists. */
        ESP_LOGE(TAG, "Secure NVS keys unavailable (partition missing or unreadable)");
        ESP_ERROR_CHECK(fail_err);
        break;
    case NVS_INIT_SECURE_ERASE:
        /* Keys are valid; only nvs_flash_secure_init itself failed to
         * decrypt the main partition. This is the genuine plaintext-to-
         * encrypted migration: old entries are unreadable. Erase and
         * re-init with the same already-valid sec_cfg; identity +
         * channels regenerate on next load and the device must be
         * re-paired (documented in the migration note). Acceptable
         * pre-alpha, first-party fleet. */
        ESP_LOGW(TAG, "Encrypted NVS unreadable (migration): erasing and reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_secure_init(&sec_cfg);
        ESP_ERROR_CHECK(ret);
        break;
    case NVS_INIT_SECURE:
        break; /* ret already ESP_OK */
    case NVS_INIT_PLAIN:
        break; /* unreachable under CONFIG_NVS_ENCRYPTION */
    }
#else
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated/new version: erasing and reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
#endif
    ESP_LOGI(TAG, "NVS initialized");

    /* Raise the OTA anti-rollback floor to the running version if higher */
    ota_rollback_note_boot();

    /* Load or generate persistent identity */
    /* Seed a hardware entropy source before any key generation. esp_random()
     * is not cryptographically secure until RF (Wi-Fi/BT) is up, and identity
     * generation runs long before that. bootloader_random_enable() turns on the
     * SAR-ADC entropy source; it MUST be disabled again before the first app
     * ADC user (battery_init, ~line 832) which shares the SAR-ADC. */
#ifndef CONFIG_IDF_TARGET_LINUX
    bootloader_random_enable();
#endif
    crypto_entropy_set_ready(true);
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
    ESP_LOGI(TAG, "Node address: %08" PRIX32 " (pubkey hash: %08" PRIX32 ")", my_addr,
             g_identity.pubkey_hash);

#ifdef CONFIG_IDF_TARGET_LINUX
    /* Emulator: connect to the gosim broker and send the hello now that the
     * real node address is known. Ordered before display_init (so the
     * boot-screen fb reaches the broker) and mesh_task_start (so the radio's
     * virtual driver has its link up). EMU_BROKER unset stays a clean
     * no-broker boot (useful standalone). The hello node id matches the
     * "Node address" log format above. */
    {
        char emu_node_id[9];
        snprintf(emu_node_id, sizeof(emu_node_id), "%08" PRIX32, my_addr);
        emu_link_set_fw_version(esp_app_get_description()->version);
        if (emu_link_connect(emu_node_id, "radio,display,buttons,gps,battery") == 0) {
            ESP_LOGI(TAG, "emu-link: attached to broker as %s", emu_node_id);
        } else {
            ESP_LOGI(TAG, "emu-link: no broker (EMU_BROKER unset or unreachable)");
        }
    }
#endif

    /* Trust-anchor campaign: load the provisioned fleet anchor pubkey (P0) and
     * this node's own endorsement cert (P1) into module memory. Both are
     * absent by default and neither is ever synthesized; a fresh node stays
     * unanchored and un-endorsed until an operator provisions them. */
    if (identity_anchor_load() == 0) {
        ESP_LOGI(TAG, "Fleet trust anchor loaded from NVS");
    }
    if (identity_endorsement_load() == 0) {
        ESP_LOGI(TAG, "Own endorsement certificate loaded from NVS");
    }

    /* Identity generated. Release the bootloader RNG before the battery ADC
     * (SAR-ADC is shared) and CLOSE the gate: there is no strong entropy source
     * again until an RF subsystem comes up, so crypto_random() must fail closed
     * in this window rather than emit weak esp_random() bytes. */
#ifndef CONFIG_IDF_TARGET_LINUX
    bootloader_random_disable();
    crypto_entropy_set_ready(false);
#else
    /* POSIX/Linux simulator: esp_random() is getentropy(), a CSPRNG that is
     * always ready; the weak-entropy window this gate fails closed against
     * does not exist on the host. */
#endif

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
        /* LVGL will handle its own rendering, just clear the display */
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
    keyboard_set_backlight(255); /* Keyboard LEDs at full brightness */
    ESP_LOGI(TAG, "=== BOOT STAGE: trackball_init ===");
    trackball_init();
#endif

    ESP_LOGI(TAG, "=== BOOT STAGE: battery_init ===");
    battery_init();
    ESP_LOGI(TAG, "Battery: %" PRIu32 " mV (%u%%)", battery_read_mv(), battery_read_pct());

    /* Init GPS on boards that advertise GPS capability */
    if (board_has_cap(BOARD_CAP_GPS)) {
        ESP_LOGI(TAG, "=== BOOT STAGE: gps_init ===");
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
        show_boot_status("GPS: searching...");
#endif
        if (gps_init(on_gps_fix, NULL) == 0) {
            ESP_LOGI(TAG, "GPS initialized (waiting for fix...)");
            /* Honor the persisted GPS-power preference (default ON). If the
             * user disabled GPS, cut power now; gps_init registered the fix
             * callback so a later Settings toggle can bring it back. */
            if (!gps_pref_get()) {
                ESP_LOGI(TAG, "GPS disabled by saved setting; cutting power");
                gps_set_enabled(false);
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
                show_boot_status("GPS: off (saved)");
#endif
            } else {
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
                show_boot_status("GPS: ok (no fix yet)");
#endif
            }
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
#endif

    /* Init alert outputs (buzzer/vibra/LED) for any board that declares the
     * capability, gated at runtime by BOARD_CAP_ALERTS rather than a single
     * board macro: indicators.c drives the PAGER's own alert pins (LED=GPIO48,
     * vibra=GPIO16, buzzer=GPIO15 LEDC), so pinning its init to the T-Deck
     * board left the pager's indicators uninitialized (s_ready stayed false)
     * and every alert a silent no-op. */
    ESP_LOGI(TAG, "=== BOOT STAGE: alerts_init ===");
    if (board_has_cap(BOARD_CAP_ALERTS)) {
        indicator_init();
        alerts_init();
        ESP_LOGI(TAG, "Alert outputs initialized (buzzer/vibra/LED)");
    }

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
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

    /* Read connectivity mode. conn_mode_get already falls back to WiFi when
     * the build carries no BLE stack; say so instead of silently booting a
     * different mode than the one persisted. */
    conn_mode_t boot_mode = conn_mode_get();
    ESP_LOGI(TAG, "Connectivity mode: %s", conn_mode_name(boot_mode));
    if (!ble_server_supported()) {
        ESP_LOGI(TAG, "BLE: unsupported in this build (stub transport); WiFi only");
    }

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
        ESP_LOGW(TAG, "SPIFFS mount failed (%s), msg persistence disabled",
                 esp_err_to_name(spiffs_ret));
    } else {
        size_t total = 0, used = 0;
        if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
            ESP_LOGI(TAG, "SPIFFS mounted: used=%u / total=%u bytes", (unsigned)used,
                     (unsigned)total);
        }
    }

    /* Init message store with persistence restore */
    msg_store_init_with_persistence();
#else
    /* Init message store (RAM-only) */
    msg_store_init();
#endif

#if CONFIG_BT_ENABLED
    if (boot_mode != CONN_MODE_BLE) {
        /* Connectivity modes are exclusive and a mode switch always reboots,
         * so a WiFi or Off boot never starts the BT controller. Hand its
         * static RAM (BT .bss/.data, internal DRAM) back to the heap instead
         * of letting it sit unused; the release is one-way per boot, which
         * the reboot-to-switch model makes safe. */
        esp_err_t bt_rel = esp_bt_mem_release(ESP_BT_MODE_BLE);
        if (bt_rel != ESP_OK) {
            ESP_LOGW(TAG, "esp_bt_mem_release failed: %s", esp_err_to_name(bt_rel));
        }
    }
#endif

    /* Init WiFi if selected */
    if (boot_mode == CONN_MODE_WIFI) {
        ESP_LOGI(TAG, "=== BOOT STAGE: wifi_init ===");
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
        show_boot_status("WiFi: starting...");
#endif
        /* Seed the per-device SoftAP password derivation before init: AP
         * mode refuses to start without it (issue #78). The Ed25519 secret
         * key never leaves this call, only its HKDF image does. */
        wifi_manager_set_ap_secret(g_identity.ed25519_private_key,
                                   sizeof(g_identity.ed25519_private_key));
        if (wifi_manager_init(my_addr) == 0) {
            /* RF subsystem up: esp_random() now reseeds from the RF entropy source. */
            crypto_entropy_set_ready(true);
            const char* ip = wifi_manager_get_ip();
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

#ifndef CONFIG_IDF_TARGET_LINUX
                ESP_LOGI(TAG, "=== BOOT STAGE: mdns_init ===");
                mdns_init();
                char hostname[32];
                snprintf(hostname, sizeof(hostname), "bramble-%04" PRIx32, my_addr & 0xFFFF);
                mdns_hostname_set(hostname);
                mdns_instance_name_set("Bramble Mesh Node");
                /* TXT records let the desktop app identify nodes before
                 * connecting: addr is the full address (the hostname only
                 * carries the low 16 bits), name is the friendly name. */
                char addr_txt[9];
                snprintf(addr_txt, sizeof(addr_txt), "%08" PRIX32, my_addr);
                const char* node_name = mesh_get_node_name();
                mdns_txt_item_t txt[2] = {
                    {"addr", addr_txt},
                    {"name", node_name},
                };
                size_t txt_count = (node_name != NULL && node_name[0] != '\0') ? 2 : 1;
                mdns_service_add("Bramble", "_bramble", "_tcp", 80, txt, txt_count);
                ESP_LOGI(TAG, "mDNS: %s._bramble._tcp", hostname);
#endif /* !CONFIG_IDF_TARGET_LINUX */
            } else {
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
                /* AP fallback. The IP is always 192.168.4.1 and the SSID is
                 * in the client's scan list, so the one thing the operator
                 * cannot get anywhere else is the password: spend the line
                 * on that. The stats screen repeats it non-transiently. */
                {
                    wifi_status_t ap_st;
                    wifi_manager_get_status(&ap_st);
                    char ap_line[22];
                    if (ap_st.ap_password[0] != '\0') {
                        snprintf(ap_line, sizeof(ap_line), "AP PW %.*s", WIFI_AP_PASSWORD_LEN,
                                 ap_st.ap_password);
                    } else {
                        snprintf(ap_line, sizeof(ap_line), "WiFi: AP 192.168.4.1");
                    }
                    show_boot_status(ap_line);
                }
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
     * NOTE: radio_init() runs inside mesh_task on CPU1; if it hangs,
     * the task watchdog (CONFIG_ESP_TASK_WDT_TIMEOUT_S) will force a reset. */
    ESP_LOGI(TAG, "=== BOOT STAGE: mesh_task_start ===");
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
    show_boot_status("Radio: SX1262...");
#endif
    mesh_task_start(&g_identity);
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
    /* Radio init runs async in mesh_task, brief wait then check state */
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
            /* RF subsystem up: esp_random() now reseeds from the RF entropy source. */
            crypto_entropy_set_ready(true);
            ble_server_start();
            /* Load (or first-boot generate) the auth token now that the BT
             * controller is running: esp_random() needs an active RF
             * subsystem for full entropy. In Wi-Fi mode ws_server_start()
             * does this; in BLE mode nothing else would. */
            ws_server_load_token();
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

    /* Start serial CLI (with JSON-RPC auto-detect). The POSIX/Linux
     * simulator has no UART/USB-serial console driver; cli.c is excluded
     * from that build. */
#ifndef CONFIG_IDF_TARGET_LINUX
    ESP_LOGI(TAG, "=== BOOT STAGE: cli_init ===");
    cli_init(&g_identity);
#endif

#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
    /* Initialize LVGL graphical UI */
    ESP_LOGI(TAG, "=== BOOT STAGE: ui_graphics_init ===");
    ui_graphics_init();

    /* Create 1ms tick timer for LVGL */
    const esp_timer_create_args_t tick_args = {.callback = lv_tick_cb, .name = "lv_tick"};
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);

    /* Create LVGL task on core 0 (core 1 runs mesh).
     *
     * IMPORTANT constraints:
     * 1. Stack MUST be in internal RAM: LVGL callbacks (Settings screen)
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
        ui_graphics_task, "ui_gfx", 10240, NULL, 2, ui_task_stack, &ui_task_tcb, 0);
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
    ui_set_gps_available(&ui, board_has_cap(BOARD_CAP_GPS));
    uint32_t last_incoming_total = msg_store_total_incoming();

    /* Render initial screen */
    ESP_LOGI(TAG, "=== BOOT STAGE: initial render ===");
    render_screen(&ui);
#endif

    ESP_LOGI(TAG, "=== BOOT STAGE: main loop start ===");

#ifdef CONFIG_IDF_TARGET_LINUX
    /* Emulator only: arm the scripted sender (no-op unless EMU_AUTO_SEND is set)
     * now that the mesh send path is live. */
    emu_node_start_autosend();
#endif

    while (1) {
        log_heap_diagnostics_periodic();
#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
        /* LVGL runs in its own task, main loop just keeps watchdog happy */
        poll_connectivity_events();
        vTaskDelay(pdMS_TO_TICKS(1000));
#else
        /* Main loop: 50ms tick (20 Hz) */
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
            /* Classic pager acknowledge: while a message alert is pending
             * (LED blinking), the FIRST button press only acknowledges it,
             * consumed, so the user does not accidentally navigate away.
             * The next press acts normally. */
            if (board_has_cap(BOARD_CAP_ALERTS) && alerts_unconfirmed()) {
                alerts_confirm();
                ESP_LOGI(TAG, "Alert acknowledged (press consumed)");
            } else {
                ui_handle_button(&ui, btn, now_ms);
            }
        }

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        /* Keyboard input, only active on compose screen */
        char key;
        while (keyboard_poll(&key)) {
            if (ui_get_screen(&ui) == SCREEN_COMPOSE) {
                if (key == '\n' || key == '\r') {
                    /* Send the message */
                    if (ui.compose_len > 0) {
                        ui.compose_buf[ui.compose_len] = '\0';
                        mesh_send_broadcast((const uint8_t*)ui.compose_buf, ui.compose_len);
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
                    /* Escape: cancel compose */
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

        /* Incoming message detection for text UI boards. Uses the monotonic
         * incoming counter: msg_store_count() saturates at the ring capacity
         * and outgoing sends must not trigger the notification path. */
        {
            uint32_t incoming_total = msg_store_total_incoming();
            if (incoming_total != last_incoming_total) {
                ui_on_message_received(&ui, now_ms);
                if (board_has_cap(BOARD_CAP_ALERTS))
                    alerts_message_received(now_ms);
                last_incoming_total = incoming_total;
            }
            ui_set_message_total(&ui, msg_store_count());
        }

        /* Feed the Nodes screen's selectable-neighbor count so ui_handle_button
         * can clamp/wrap the cursor without a mesh dependency (see
         * selectable_neighbor_count / selectable_neighbor_addr for the shared
         * "selectable neighbor" definition the render side also uses). Cursor
         * clamping only matters on NODES, so skip the mutexed state copy on
         * every other screen; render_screen re-fetches mesh state for its own
         * NODES draw, so there is no staleness from gating this. */
        if (ui_get_screen(&ui) == SCREEN_NODES) {
            mesh_shared_state_t mesh_now;
            mesh_get_state(&mesh_now);
            ui_set_node_total(&ui, selectable_neighbor_count(&mesh_now.neighbors));
        }

        /* Handle settings confirmation */
        if (ui.settings_confirmed) {
            ui.settings_confirmed = false;
            ui.settings_editing = false;
            if (ui.settings_item_cursor == UI_SETTINGS_ITEM_CONN_MODE) {
                conn_mode_t new_mode = conn_mode_from_ui_index(ui.settings_cursor);
                conn_mode_t old_mode = conn_mode_get();
                if (new_mode == CONN_MODE_BLE && !ble_server_supported()) {
                    /* Honest refusal: this build has no BLE stack, so a
                     * switch would reboot into a node with no transport. */
                    ESP_LOGW(TAG, "BLE not included in this build; mode unchanged");
                    display_clear();
                    const char* msg = "BLE: not in build";
                    int msg_x = (DISPLAY_WIDTH - (int)strlen(msg) * FONT_W) / 2;
                    display_draw_text(msg_x, DISPLAY_HEIGHT / 2 - FONT_H / 2, msg);
                    display_flush();
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    ui.screen_dirty = true;
                } else if (new_mode != old_mode) {
                    conn_mode_set(new_mode);
                    ESP_LOGI(TAG, "Connectivity mode changed to %d, rebooting...", new_mode);

                    /* Show confirmation before reboot */
                    display_clear();

                    const char* msg1 = "Mode changed:";
                    int msg1_x = (DISPLAY_WIDTH - strlen(msg1) * FONT_W) / 2;
                    int msg1_y = DISPLAY_HEIGHT / 4;
                    display_draw_text(msg1_x, msg1_y, msg1);

                    int mode_w = strlen(conn_mode_name(new_mode)) * FONT_W * 2;
                    int mode_x = (DISPLAY_WIDTH - mode_w) / 2;
                    int mode_y = msg1_y + FONT_H + 8;
                    display_draw_text_large(mode_x, mode_y, conn_mode_name(new_mode));

                    const char* msg2 = "Rebooting...";
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
                    static const char* loc_names[] = {"Off", "Coarse", "Exact"};
                    ESP_LOGI(TAG, "Location sharing set to %s", loc_names[new_loc]);
                }
                ui.screen_dirty = true;
            } else if (ui.settings_item_cursor == UI_SETTINGS_ITEM_GPS) {
                bool new_gps = (ui.settings_cursor == 1);
                bool old_gps = gps_pref_get();
                if (new_gps != old_gps) {
                    gps_pref_set(new_gps);
                    gps_set_enabled(new_gps);
                    ESP_LOGI(TAG, "GPS %s via settings", new_gps ? "enabled" : "disabled");
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

        /* Handle SAS-verify confirmation. s_nodes_detail_addr/valid are set by
         * the detail render, so this always applies to exactly the peer the
         * user was looking at when they confirmed, even if the neighbor list
         * reordered between the button press and this check. */
        if (ui.node_verify_confirmed) {
            ui.node_verify_confirmed = false;
            if (s_nodes_detail_valid)
                mesh_set_peer_verified(s_nodes_detail_addr, true);
        }

        /* Check inactivity timeout */
        ui_check_timeout(&ui, now_ms);

        /* Redraw if needed */
        if (ui_needs_redraw(&ui)) {
            render_screen(&ui);
            ui_mark_drawn(&ui);
        }

        /* Periodic refresh of the main screen's uptime counter. On a fast
         * display (OLED/LCD) this is a cheap 1 Hz tick. On e-paper it would
         * flush the panel every second (busy roughly half the time) and force
         * a ghost-clearing full-refresh flicker every ~10s, wearing the panel
         * and looking broken, so slow it drastically there: the pager updates
         * its status screen on real events (messages, neighbors), not a
         * live-ticking clock. */
        const uint32_t uptime_refresh_ms = board_has_cap(BOARD_CAP_DISPLAY_EPAPER) ? 60000u : 1000u;
        if (ui_get_screen(&ui) == SCREEN_MAIN && (now_ms % uptime_refresh_ms) < 50) {
            render_main_screen(&ui);
        }

        /* Alert outputs: advance the beep/vibra pattern and keep the
         * notification LED lit while unread messages exist. */
        if (board_has_cap(BOARD_CAP_ALERTS))
            alerts_tick(now_ms);

        vTaskDelay(pdMS_TO_TICKS(50));
#endif
    }
}
