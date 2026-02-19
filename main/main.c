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
    const char *ver = "v0.1.0-dev";
    int ver_w = strlen(ver) * FONT_W;
    display_draw_text((DISPLAY_WIDTH - ver_w) / 2, div_y + 20, ver);

    display_flush();
}

/* ── Screen renderers ───────────────────────────────────────────────── */

static bramble_identity_t g_identity;
static uint32_t my_addr = 0;
static uint32_t boot_time_ms = 0;

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

    /* Get live mesh state (static to avoid stack overflow — neighbor_table_t is ~1.8KB) */
    static mesh_shared_state_t mesh;
    mesh_get_state(&mesh);

    /* Neighbors + radio status */
    int n = neighbor_count(&mesh.neighbors);
    if (mesh.radio_ok) {
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
                 mesh.last_rx_rssi, mesh.last_rx_snr);
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

    /* Beacon counts */
    snprintf(line, sizeof(line), "TX:%" PRIu32 " RX:%" PRIu32,
             mesh.beacon_tx_count, mesh.beacon_rx_count);
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
                const char *arrow = (m->direction == MSG_DIR_OUTGOING ||
                                     m->direction == MSG_DIR_BROADCAST_OUT)
                                    ? ">" : "<";
                snprintf(line, sizeof(line), "%s %.*s", arrow, CHARS_PER_LINE - 3, m->text);
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
        display_draw_text(2, HEADER_Y, "Nodes");
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);
        static mesh_shared_state_t mesh_n;
        mesh_get_state(&mesh_n);
        int cnt = neighbor_count(&mesh_n.neighbors);
        if (cnt == 0) {
            int no_nbr_y = (DISPLAY_HEIGHT - FONT_H) / 2;
            const char *no_nbr = "(no neighbors yet)";
            int no_nbr_x = (DISPLAY_WIDTH - strlen(no_nbr) * FONT_W) / 2;
            display_draw_text(no_nbr_x, no_nbr_y, no_nbr);
        } else {
            char nl[64];
            int y = CONTENT_Y;
            for (int i = 0; i < mesh_n.neighbors.count && y < FOOTER_Y; i++) {
                neighbor_entry_t *e = &mesh_n.neighbors.entries[i];
                if (e->addr == 0) continue;
                snprintf(nl, sizeof(nl), "%08" PRIX32 " %ddBm", e->addr, e->rssi);
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
            /* Mode selection UI */
            int y = CONTENT_Y;
            display_draw_text(2, y, "Connectivity Mode:");
            y += LINE_H + 4;
            static const char *mode_names[] = {"WiFi", "BLE", "WiFi + BLE"};
            conn_mode_t current = conn_mode_get();
            for (int i = 0; i < CONN_MODE_COUNT; i++) {
                char ml[32];
                const char *arrow = (i == ui->settings_cursor) ? ">" : " ";
                const char *mark = (i == (int)current) ? " *" : "";
                snprintf(ml, sizeof(ml), "%s %s%s", arrow, mode_names[i], mark);
                display_draw_text(2, y, ml);
                y += LINE_H;
            }
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
            display_draw_text(2, FOOTER_Y, "^v choose  [o]OK  [<]cancel");
#else
            display_draw_text(2, FOOTER_Y, "[hold]OK [2x]cancel");
#endif
        } else {
            char line[64];
            int y = CONTENT_Y;
            snprintf(line, sizeof(line), "Addr: %08" PRIX32, my_addr);
            display_draw_text(2, y, line);
            y += LINE_H;

            conn_mode_t cur_mode = conn_mode_get();
            static const char *mnames[] = {"WiFi", "BLE", "WiFi+BLE"};
            snprintf(line, sizeof(line), "Mode: %s", mnames[cur_mode]);
            display_draw_text(2, y, line);
            y += LINE_H;

            const char *ip = wifi_manager_get_ip();
            if (ip && ip[0] != '\0') {
                snprintf(line, sizeof(line), "IP: %s", ip);
                display_draw_text(2, y, line);
            } else {
                display_draw_text(2, y, "IP: (no WiFi)");
            }
            y += LINE_H;

            if (cur_mode == CONN_MODE_BLE || cur_mode == CONN_MODE_BOTH) {
                display_draw_text(2, y, "BLE: advertising");
            } else {
                display_draw_text(2, y, "BLE: off");
            }
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
            display_draw_text(2, FOOTER_Y, "[o] edit  < > navigate");
#else
            display_draw_text(2, FOOTER_Y, "[hold] change mode");
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
        /* Heltec: About screen (existing code) */
        display_draw_text(2, HEADER_Y, "About");
        display_hline(0, DIVIDER_Y, DISPLAY_WIDTH);

        char line[64];
        int y = CONTENT_Y;
        display_draw_text(2, y, "Bramble Mesh v0.1");
        y += LINE_H + 4;

        uint8_t bpct = battery_read_pct();
        uint32_t bmv = battery_read_mv();
        if (bmv > 0) {
            snprintf(line, sizeof(line), "Batt: %u%% (%"PRIu32"mV)", bpct, bmv);
        } else {
            snprintf(line, sizeof(line), "Power: USB");
        }
        display_draw_text(2, y, line);
        y += LINE_H;

        snprintf(line, sizeof(line), "Heap: %uKB",
                 (unsigned)(esp_get_free_heap_size() / 1024));
        display_draw_text(2, y, line);
        y += LINE_H;

        static mesh_shared_state_t about_mesh;
        mesh_get_state(&about_mesh);
        snprintf(line, sizeof(line), "TX:%"PRIu32" RX:%"PRIu32,
                 about_mesh.packets_tx, about_mesh.packets_rx);
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
#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
        /* LVGL will handle its own rendering — just clear the display */
        ESP_LOGI(TAG, "=== BOOT STAGE: clear display for LVGL ===");
        display_clear();
        display_flush();
#else
        ESP_LOGI(TAG, "=== BOOT STAGE: show_splash ===");
        show_splash();
        ESP_LOGI(TAG, "Splash screen displayed — waiting 2 s");
        vTaskDelay(pdMS_TO_TICKS(2000)); /* Show splash for 2 seconds */
#endif
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
    
    /* Create LVGL task on core 1 (core 0 runs mesh) */
    xTaskCreatePinnedToCore(ui_graphics_task, "ui_gfx", 8192, NULL, 5, NULL, 1);
#else
    /* Init text UI state machine (Heltec and other non-graphical boards) */
    ESP_LOGI(TAG, "=== BOOT STAGE: ui_init ===");
    ui_state_t ui;
    ui_init(&ui);

    /* Render initial screen */
    ESP_LOGI(TAG, "=== BOOT STAGE: initial render ===");
    render_screen(&ui);
#endif

    ESP_LOGI(TAG, "=== BOOT STAGE: main loop start ===");

    while (1) {
#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
        /* LVGL runs in its own task — main loop just keeps watchdog happy */
        vTaskDelay(pdMS_TO_TICKS(1000));
#else
        /* Main loop — 50ms tick (20 Hz) */
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

        /* Handle connectivity mode change confirmation */
        if (ui.settings_confirmed) {
            ui.settings_confirmed = false;
            ui.settings_editing = false;
            conn_mode_t new_mode = (conn_mode_t)ui.settings_cursor;
            conn_mode_t old_mode = conn_mode_get();
            if (new_mode != old_mode) {
                conn_mode_set(new_mode);
                ESP_LOGI(TAG, "Connectivity mode changed to %d, rebooting...", new_mode);

                /* Show confirmation before reboot */
                display_clear();
                static const char *mnames[] = {"WiFi", "BLE", "WiFi+BLE"};
                
                /* Center the confirmation text */
                const char *msg1 = "Mode changed:";
                int msg1_x = (DISPLAY_WIDTH - strlen(msg1) * FONT_W) / 2;
                int msg1_y = DISPLAY_HEIGHT / 4;
                display_draw_text(msg1_x, msg1_y, msg1);
                
                /* Mode name in large text, centered */
                int mode_w = strlen(mnames[new_mode]) * FONT_W * 2;
                int mode_x = (DISPLAY_WIDTH - mode_w) / 2;
                int mode_y = msg1_y + FONT_H + 8;
                display_draw_text_large(mode_x, mode_y, mnames[new_mode]);
                
                /* Rebooting message */
                const char *msg2 = "Rebooting...";
                int msg2_x = (DISPLAY_WIDTH - strlen(msg2) * FONT_W) / 2;
                int msg2_y = mode_y + LARGE_FONT_H + 8;
                display_draw_text(msg2_x, msg2_y, msg2);
                
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
#endif
    }
}
