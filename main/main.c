#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
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
#include "mdns.h"
#include "ble_server.h"
#include "esp_system.h"
#include "battery.h"
#include "board_config.h"
#include "keyboard.h"
#include "trackball.h"
#include "location.h"

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#include "gps.h"
#include "sdcard.h"
#endif

static const char *TAG = "bramble";

/* ── Location manager ───────────────────────────────────────────────── */

static location_manager_t g_location_mgr;

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
static void on_gps_fix(const bramble_position_t *pos, void *ctx) {
    location_manager_t *mgr = (location_manager_t *)ctx;
    location_set_position(mgr, pos);
    ESP_LOGI(TAG, "GPS position updated: lat=%.6f lon=%.6f alt=%d",
             pos->latitude_e7 / 1e7, pos->longitude_e7 / 1e7, pos->altitude_m);
}
#endif

/* ── Connectivity mode (NVS-persisted) ──────────────────────────────── */

conn_mode_t conn_mode_get(void) {
    nvs_handle_t nvs;
    uint8_t mode = CONN_MODE_BOTH; /* default: both WiFi and BLE */
    if (nvs_open("bramble", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "conn_mode", &mode);
        nvs_close(nvs);
    }
    if (mode >= CONN_MODE_COUNT) mode = CONN_MODE_BOTH;
    return (conn_mode_t)mode;
}

static void conn_mode_set(conn_mode_t mode) {
    nvs_handle_t nvs;
    if (nvs_open("bramble", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "conn_mode", (uint8_t)mode);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* ── Splash screen ──────────────────────────────────────────────────── */

static void show_splash(void) {
    display_clear();

    /* "BRAMBLE" in large text, centered */
    display_draw_text_large(16, 8, "BRAMBLE");

    /* Divider line */
    display_hline(10, 30, 108);

    /* Tagline */
    display_draw_text(16, 36, "LoRa Mesh Network");

    /* Version */
    display_draw_text(34, 50, "v0.1.0-dev");

    display_flush();
}

/* ── Screen renderers ───────────────────────────────────────────────── */

static bramble_identity_t g_identity;
static uint32_t my_addr = 0;
static uint32_t boot_time_ms = 0;

static void render_main_screen(void) {
    display_clear();

    /* Header — name + battery */
    {
        uint8_t bpct = battery_read_pct();
        char hdr[32];
        if (bpct > 0) {
            snprintf(hdr, sizeof(hdr), "Bramble      %3u%%", bpct);
        } else {
            snprintf(hdr, sizeof(hdr), "Bramble      USB");
        }
        display_draw_text(0, 0, hdr);
    }
    display_hline(0, 10, 128);

    /* Node address */
    char line[48];
    snprintf(line, sizeof(line), "Node: %08" PRIX32, my_addr);
    display_draw_text(0, 14, line);

    /* Get live mesh state (static to avoid stack overflow — neighbor_table_t is ~1.8KB) */
    static mesh_shared_state_t mesh;
    mesh_get_state(&mesh);

    /* Neighbors */
    int n = neighbor_count(&mesh.neighbors);
    if (mesh.radio_ok) {
        snprintf(line, sizeof(line), "Peers: %d", n);
        display_draw_text(0, 24, line);
    } else {
        display_draw_text(0, 24, "Radio: initializing...");
    }

    /* WiFi IP address (if connected), else last RX signal */
    const char *ip = wifi_manager_get_ip();
    if (ip && ip[0] != '\0') {
        snprintf(line, sizeof(line), "IP: %s", ip);
        display_draw_text(0, 34, line);
    } else if (n > 0) {
        snprintf(line, sizeof(line), "RSSI:%d SNR:%d",
                 mesh.last_rx_rssi, mesh.last_rx_snr);
        display_draw_text(0, 34, line);
    }

    /* Uptime */
    uint32_t up_sec = (uint32_t)((esp_timer_get_time() / 1000000ULL) -
                                  (boot_time_ms / 1000));
    char uptime[32];
    ui_format_uptime(up_sec, uptime, sizeof(uptime));
    snprintf(line, sizeof(line), "Up: %s", uptime);
    display_draw_text(0, 44, line);

    /* Beacon counts */
    snprintf(line, sizeof(line), "TX:%" PRIu32 " RX:%" PRIu32,
             mesh.beacon_tx_count, mesh.beacon_rx_count);
    display_draw_text(0, 56, line);

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
        display_draw_text(0, 0, hdr);
        display_hline(0, 10, 128);

        if (mcount == 0) {
            display_draw_text(0, 24, "(no messages yet)");
        } else {
            /* Show last 3 messages (newest at bottom), 14px per line */
            int start = mcount > 3 ? mcount - 3 : 0;
            int y = 14;
            for (int i = start; i < mcount && y < 54; i++) {
                const stored_msg_t *m = msg_store_get(i);
                if (!m) continue;
                char line[24];  /* 21 chars fit on 128px at 6px/char */
                const char *arrow = (m->direction == MSG_DIR_OUTGOING ||
                                     m->direction == MSG_DIR_BROADCAST_OUT)
                                    ? ">" : "<";
                snprintf(line, sizeof(line), "%s %.20s", arrow, m->text);
                display_draw_text(0, y, line);
                y += 14;
            }
        }
        display_draw_text(0, 56, "[press] next screen");
        display_flush();
        break;
    }
    case SCREEN_NODES: {
        display_clear();
        display_draw_text(0, 0, "Nodes");
        display_hline(0, 10, 128);
        static mesh_shared_state_t mesh_n;
        mesh_get_state(&mesh_n);
        int cnt = neighbor_count(&mesh_n.neighbors);
        if (cnt == 0) {
            display_draw_text(0, 24, "(no neighbors yet)");
        } else {
            char nl[48];
            int y = 14;
            for (int i = 0; i < mesh_n.neighbors.count && y < 56; i++) {
                neighbor_entry_t *e = &mesh_n.neighbors.entries[i];
                if (e->addr == 0) continue;
                snprintf(nl, sizeof(nl), "%08" PRIX32 " %ddBm", e->addr, e->rssi);
                display_draw_text(0, y, nl);
                y += 10;
            }
        }
        display_draw_text(0, 56, "[press] next screen");
        display_flush();
        break;
    }
    case SCREEN_SETTINGS: {
        display_clear();
        display_draw_text(0, 0, "Settings");
        display_hline(0, 10, 128);

        if (ui->settings_editing) {
            /* Mode selection UI */
            display_draw_text(0, 14, "Connectivity Mode:");
            static const char *mode_names[] = {"WiFi", "BLE", "WiFi + BLE"};
            conn_mode_t current = conn_mode_get();
            for (int i = 0; i < CONN_MODE_COUNT; i++) {
                char ml[32];
                const char *arrow = (i == ui->settings_cursor) ? ">" : " ";
                const char *mark = (i == (int)current) ? " *" : "";
                snprintf(ml, sizeof(ml), "%s %s%s", arrow, mode_names[i], mark);
                display_draw_text(0, 26 + i * 12, ml);
            }
            display_draw_text(0, 56, "[hold]OK [2x]cancel");
        } else {
            char line[32];
            snprintf(line, sizeof(line), "Addr: %08" PRIX32, my_addr);
            display_draw_text(0, 14, line);

            conn_mode_t cur_mode = conn_mode_get();
            static const char *mnames[] = {"WiFi", "BLE", "WiFi+BLE"};
            snprintf(line, sizeof(line), "Mode: %s", mnames[cur_mode]);
            display_draw_text(0, 24, line);

            const char *ip = wifi_manager_get_ip();
            if (ip && ip[0] != '\0') {
                snprintf(line, sizeof(line), "IP: %s", ip);
                display_draw_text(0, 34, line);
            } else {
                display_draw_text(0, 34, "IP: (no WiFi)");
            }

            if (cur_mode == CONN_MODE_BLE || cur_mode == CONN_MODE_BOTH) {
                display_draw_text(0, 44, "BLE: advertising");
            } else {
                display_draw_text(0, 44, "BLE: off");
            }
            display_draw_text(0, 56, "[hold] change mode");
        }
        display_flush();
        break;
    }
    case SCREEN_COMPOSE: {
        /* Quick status / about screen */
        display_clear();
        display_draw_text(0, 0, "About");
        display_hline(0, 10, 128);

        char line[48];
        display_draw_text(0, 14, "Bramble Mesh v0.1");

        uint8_t bpct = battery_read_pct();
        uint32_t bmv = battery_read_mv();
        if (bmv > 0) {
            snprintf(line, sizeof(line), "Batt: %u%% (%"PRIu32"mV)", bpct, bmv);
        } else {
            snprintf(line, sizeof(line), "Power: USB");
        }
        display_draw_text(0, 26, line);

        snprintf(line, sizeof(line), "Heap: %uKB",
                 (unsigned)(esp_get_free_heap_size() / 1024));
        display_draw_text(0, 38, line);

        static mesh_shared_state_t about_mesh;
        mesh_get_state(&about_mesh);
        snprintf(line, sizeof(line), "TX:%"PRIu32" RX:%"PRIu32,
                 about_mesh.packets_tx, about_mesh.packets_rx);
        display_draw_text(0, 50, line);

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
        ESP_LOGI(TAG, "=== BOOT STAGE: show_splash ===");
        show_splash();
        ESP_LOGI(TAG, "Splash screen displayed — waiting 2 s");
        vTaskDelay(pdMS_TO_TICKS(2000)); /* Show splash for 2 seconds */
    }

    /* Init button */
    ESP_LOGI(TAG, "=== BOOT STAGE: button_init ===");
    button_init();

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    /* Init keyboard and trackball (T-Deck Plus only) */
    ESP_LOGI(TAG, "=== BOOT STAGE: keyboard_init ===");
    keyboard_init();
    ESP_LOGI(TAG, "=== BOOT STAGE: trackball_init ===");
    trackball_init();
#endif

    ESP_LOGI(TAG, "=== BOOT STAGE: battery_init ===");
    battery_init();
    ESP_LOGI(TAG, "Battery: %" PRIu32 " mV (%u%%)", battery_read_mv(), battery_read_pct());

    /* Init location manager */
    ESP_LOGI(TAG, "=== BOOT STAGE: location_init ===");
    location_init(&g_location_mgr);

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    /* Init GPS (T-Deck Plus only) */
    ESP_LOGI(TAG, "=== BOOT STAGE: gps_init ===");
    if (gps_init(on_gps_fix, &g_location_mgr) == 0) {
        ESP_LOGI(TAG, "GPS initialized (waiting for fix...)");
    } else {
        ESP_LOGW(TAG, "GPS init failed or not available");
    }

    /* Init SD card (T-Deck Plus only) */
    ESP_LOGI(TAG, "=== BOOT STAGE: sdcard_init ===");
    if (sdcard_init() == 0) {
        ESP_LOGI(TAG, "SD card mounted at %s", sdcard_get_mount_point());
    } else {
        ESP_LOGW(TAG, "SD card init failed or not present");
    }
#endif

    /* Read connectivity mode */
    conn_mode_t boot_mode = conn_mode_get();
    static const char *mode_str[] = {"WiFi", "BLE", "WiFi+BLE"};
    ESP_LOGI(TAG, "Connectivity mode: %s", mode_str[boot_mode]);

    /* Init RPC dispatcher and register methods BEFORE transports
     * so ws_server/ble notify registrations aren't wiped by rpc_init() */
    ESP_LOGI(TAG, "=== BOOT STAGE: rpc_init ===");
    rpc_init();
    rpc_methods_init(&g_identity);

    /* Init message store */
    msg_store_init();

    /* Init WiFi if mode includes it */
    if (boot_mode == CONN_MODE_WIFI || boot_mode == CONN_MODE_BOTH) {
        ESP_LOGI(TAG, "=== BOOT STAGE: wifi_init ===");
        if (wifi_manager_init() == 0) {
            const char *ip = wifi_manager_get_ip();
            if (ip[0] != '\0') {
                ESP_LOGI(TAG, "WiFi ready: %s", ip);

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
            }
        } else {
            ESP_LOGW(TAG, "WiFi init failed");
        }
    } else {
        ESP_LOGI(TAG, "WiFi disabled by connectivity mode");
    }

    /* Start mesh task (radio + beacons on CPU1).
     * NOTE: radio_init() runs inside mesh_task on CPU1 — if it hangs,
     * the task watchdog (CONFIG_ESP_TASK_WDT_TIMEOUT_S) will force a reset. */
    ESP_LOGI(TAG, "=== BOOT STAGE: mesh_task_start ===");
    mesh_task_start(&g_identity);

    /* Start BLE GATT server if mode includes it */
    if (boot_mode == CONN_MODE_BLE || boot_mode == CONN_MODE_BOTH) {
        ESP_LOGI(TAG, "=== BOOT STAGE: ble_init ===");
        if (ble_server_init() == 0) {
            ble_server_start();
            ESP_LOGI(TAG, "BLE server started");
        } else {
            ESP_LOGW(TAG, "BLE init failed");
        }
    } else {
        ESP_LOGI(TAG, "BLE disabled by connectivity mode");
    }

    /* Start serial CLI (with JSON-RPC auto-detect) */
    ESP_LOGI(TAG, "=== BOOT STAGE: cli_init ===");
    cli_init(&g_identity);

    /* Init UI state machine */
    ESP_LOGI(TAG, "=== BOOT STAGE: ui_init ===");
    ui_state_t ui;
    ui_init(&ui);

    /* Render initial screen */
    ESP_LOGI(TAG, "=== BOOT STAGE: initial render ===");
    render_screen(&ui);

    ESP_LOGI(TAG, "=== BOOT STAGE: main loop start (UI on CPU0, mesh on CPU1) ===");

    /* Main loop — 50ms tick (20 Hz) */
    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

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

        /* Handle connectivity mode change confirmation */
        if (ui.settings_confirmed) {
            ui.settings_confirmed = false;
            ui.settings_editing = false;
            conn_mode_t new_mode = (conn_mode_t)ui.settings_cursor;
            conn_mode_t old_mode = conn_mode_get();
            if (new_mode != old_mode) {
                conn_mode_set(new_mode);
                ESP_LOGI(TAG, "Connectivity mode changed to %d, rebooting...", new_mode);

                /* Show confirmation on OLED before reboot */
                display_clear();
                static const char *mnames[] = {"WiFi", "BLE", "WiFi+BLE"};
                display_draw_text(16, 16, "Mode changed:");
                display_draw_text_large(16, 30, mnames[new_mode]);
                display_draw_text(16, 50, "Rebooting...");
                display_flush();
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            } else {
                /* Same mode — just exit edit */
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
    }
}
