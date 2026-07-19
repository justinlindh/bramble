/**
 * Bramble serial CLI: interactive command interface over UART.
 *
 * Commands:
 *   send <addr> <message>  - Send encrypted DM to a node (hex address)
 *   broadcast <message>    - Send on public channel (Bramble Common)
 *   peers                  - List known neighbors
 *   status                 - Show node status
 *   help                   - Show commands
 */

#include "cli.h"
#include "mesh_task.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include "wifi_manager.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "linenoise/linenoise.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static const char* TAG = "cli";
static bramble_identity_t* s_identity;

/* ── Command: peers ─────────────────────────────────────────────────── */

static int cmd_peers(int argc, char** argv) {
    (void)argc;
    (void)argv;
    static mesh_shared_state_t state;
    mesh_get_state(&state);

    int count = neighbor_count(&state.neighbors);
    printf("Neighbors: %d\n", count);
    for (int i = 0; i < state.neighbors.count; i++) {
        neighbor_entry_t* e = &state.neighbors.entries[i];
        if (e->addr == 0)
            continue;
        printf("  %08" PRIX32 "  RSSI:%d  SNR:%d\n", e->addr, e->rssi, e->snr);
    }
    return 0;
}

/* ── Command: status ────────────────────────────────────────────────── */

static int cmd_status(int argc, char** argv) {
    (void)argc;
    (void)argv;
    static mesh_shared_state_t state;
    mesh_get_state(&state);

    printf("Node:     %08" PRIX32 "\n", s_identity->address);
    printf("Radio:    %s\n", state.radio_ok ? "OK" : "FAILED");
    printf("Peers:    %d\n", neighbor_count(&state.neighbors));
    printf("Beacons:  TX:%" PRIu32 " RX:%" PRIu32 "\n", state.beacon_tx_count,
           state.beacon_rx_count);
    printf("Packets:  TX:%" PRIu32 " RX:%" PRIu32 "\n", state.packets_tx, state.packets_rx);
    if (state.last_rx_rssi != 0) {
        printf("Last RX:  RSSI:%d SNR:%d\n", state.last_rx_rssi, state.last_rx_snr);
    }
    return 0;
}

/* ── Command: broadcast ─────────────────────────────────────────────── */

static int cmd_broadcast(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: broadcast <message>\n");
        return 1;
    }

    /* Concatenate all args as message */
    char msg[256];
    int pos = 0;
    for (int i = 1; i < argc && pos < (int)sizeof(msg) - 1; i++) {
        if (i > 1)
            msg[pos++] = ' ';
        int len = strlen(argv[i]);
        if (pos + len >= (int)sizeof(msg) - 1)
            len = sizeof(msg) - 1 - pos;
        memcpy(msg + pos, argv[i], len);
        pos += len;
    }
    msg[pos] = '\0';

    printf("Broadcasting: \"%s\"\n", msg);
    int ret = mesh_send_broadcast((const uint8_t*)msg, pos);
    if (ret == 0) {
        printf("Sent OK\n");
    } else {
        printf("Send failed: %d\n", ret);
    }
    return ret;
}

/* ── Command: send ──────────────────────────────────────────────────── */

static int cmd_send(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: send <hex-addr> <message>\n");
        printf("  e.g.: send 6EEA8967 hello there\n");
        return 1;
    }

    uint32_t dest_addr;
    if (sscanf(argv[1], "%" SCNx32, &dest_addr) != 1) {
        printf("Invalid address: %s (use hex, e.g. 6EEA8967)\n", argv[1]);
        return 1;
    }

    /* Concatenate remaining args as message */
    char msg[256];
    int pos = 0;
    for (int i = 2; i < argc && pos < (int)sizeof(msg) - 1; i++) {
        if (i > 2)
            msg[pos++] = ' ';
        int len = strlen(argv[i]);
        if (pos + len >= (int)sizeof(msg) - 1)
            len = sizeof(msg) - 1 - pos;
        memcpy(msg + pos, argv[i], len);
        pos += len;
    }
    msg[pos] = '\0';

    printf("Sending to %08" PRIX32 ": \"%s\"\n", dest_addr, msg);
    int ret = mesh_send_message(dest_addr, (const uint8_t*)msg, pos);
    if (ret == 0) {
        printf("Sent OK\n");
    } else {
        printf("Send failed: %d\n", ret);
    }
    return ret;
}

/* ── Command: wifi ──────────────────────────────────────────────────── */

static int cmd_wifi(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("  wifi status              Show WiFi status\n");
        printf("  wifi set <ssid> <pass>   Save WiFi credentials (persists across reflash)\n");
        printf("  wifi clear               Clear saved credentials\n");
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        wifi_status_t st;
        wifi_manager_get_status(&st);
        const char* mode_str = st.mode == BRAMBLE_WIFI_STATION ? "Station"
                               : st.mode == BRAMBLE_WIFI_AP    ? "AP"
                                                               : "Off";
        printf("Mode:  %s\n", mode_str);
        printf("SSID:  %s\n", st.ssid);
        printf("IP:    %s\n", st.ip_addr);

        /* The AP password is per-device and derived, so the serial console is
         * the discovery path on a board with no display. Only shown while the
         * AP is actually up, and only over the physically attached console. */
        if (st.mode == BRAMBLE_WIFI_AP && st.ap_password[0] != '\0') {
            printf("AP PW: %s\n", st.ap_password);
        }

        /* Show saved NVS creds (SSID only, not password) */
        char nvs_ssid[33] = {0};
        char nvs_pass[65] = {0};
        if (wifi_manager_nvs_get_creds(nvs_ssid, sizeof(nvs_ssid), nvs_pass, sizeof(nvs_pass)) ==
            0) {
            printf("Saved: %s (in NVS)\n", nvs_ssid);
        } else {
            printf("Saved: (none)\n");
        }
    } else if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            printf("Usage: wifi set <ssid> [password]\n");
            return 1;
        }
        const char* ssid = argv[2];
        const char* pass = argc >= 4 ? argv[3] : "";
        if (wifi_manager_nvs_set_creds(ssid, pass) == 0) {
            printf("WiFi credentials saved. Reboot to connect.\n");
            printf("  SSID: %s\n", ssid);
            printf("  Pass: %s\n", pass[0] ? "****" : "(open)");
        } else {
            printf("Failed to save credentials.\n");
            return 1;
        }
    } else if (strcmp(argv[1], "clear") == 0) {
        if (wifi_manager_nvs_clear_creds() == 0) {
            printf("WiFi credentials cleared. Reboot to use AP mode.\n");
        } else {
            printf("Failed to clear credentials.\n");
            return 1;
        }
    } else {
        printf("Unknown wifi subcommand: %s\n", argv[1]);
        return 1;
    }
    return 0;
}

/* ── Command: name ──────────────────────────────────────────────────── */

static int cmd_name(int argc, char** argv) {
    if (argc < 2) {
        const char* current = mesh_get_node_name();
        if (current && current[0]) {
            printf("Node name: %s\n", current);
        } else {
            printf("No name set. Usage: name <your-name>\n");
        }
        return 0;
    }

    /* Join all args as the name (allows spaces) */
    static char name_buf[64];
    name_buf[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            strncat(name_buf, " ", sizeof(name_buf) - strlen(name_buf) - 1);
        strncat(name_buf, argv[i], sizeof(name_buf) - strlen(name_buf) - 1);
    }

    int ret = mesh_set_node_name_persist(name_buf);
    if (ret == 0) {
        printf("Name set: %s\n", name_buf);
    } else {
        printf("Failed to set name: %d\n", ret);
    }
    return ret;
}

/* ── Command: reboot ────────────────────────────────────────────────── */

static int cmd_reboot(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("Rebooting...\n");
    fflush(stdout);
    mesh_reboot_delayed(500);
    return 0;
}

/* ── Command: help ──────────────────────────────────────────────────── */

static int cmd_help(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("Bramble CLI commands:\n");
    printf("  broadcast <msg>        Send on public channel\n");
    printf("  send <addr> <msg>      Send encrypted to address\n");
    printf("  peers                  List neighbors\n");
    printf("  status                 Node status\n");
    printf("  name [name]            Show or set node name\n");
    printf("  wifi status|set|clear  WiFi management\n");
    printf("  reboot                 Restart device\n");
    printf("  help                   This help\n");
    return 0;
}

/* ── UART notification callback for JSON-RPC ────────────────────────── */

static void uart_notify_cb(const char* json, size_t len, void* ctx) {
    (void)ctx;
    (void)len;
    printf("%s\n", json);
    fflush(stdout);
}

/* ── CLI task ───────────────────────────────────────────────────────── */

static void cli_task(void* param) {
    (void)param;

    /* Disable buffering on stdin/stdout */
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Tell linenoise to handle backspace etc */
    linenoiseSetMultiLine(1);
    linenoiseSetDumbMode(1); /* No escape codes, works over plain serial */

    printf("\n");
    printf("=================================\n");
    printf("  Bramble Mesh CLI\n");
    printf("  Node: %08" PRIX32 "\n", s_identity->address);
    printf("  Type 'help' for commands\n");
    printf("=================================\n");

    /* Disable task watchdog for CLI task (linenoise blocks on input) */
    esp_task_wdt_delete(NULL);

    /* JSON-RPC response buffer. The old 2 KB lived on this task's stack and
     * silently swallowed anything bigger: a full 20-message bramble.getMessages
     * dump, or a screenshot chunk over ~1.5 KB, produced NO output at all, so
     * the caller could only see a serial timeout. 16 KB from PSRAM (falling back
     * to internal RAM on boards without it) covers a full message store, and an
     * overflow past even that now comes back as an explicit "response too large"
     * error from rpc_dispatch rather than silence. */
    const size_t resp_cap = 16 * 1024;
    char* response = heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response) {
        response = malloc(resp_cap);
    }
    if (!response) {
        ESP_LOGE(TAG, "CLI response buffer alloc failed (%u bytes)", (unsigned)resp_cap);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        char* line = linenoise("bramble> ");
        if (line == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (strlen(line) > 0) {
            /* Skip history for JSON-RPC commands: they have unique IDs
             * and would fill the 100-entry history with unreusable entries,
             * each leaking a strdup into PSRAM until rotation. */
            if (line[0] != '{') {
                linenoiseHistoryAdd(line);
            }

            if (line[0] == '{') {
                /* JSON-RPC mode. Serial dispatch is full-privilege BY
                 * DESIGN: physical USB access is the pairing bootstrap
                 * that retrieves the device auth token (`bramble pair`).
                 * See docs/SECURITY-MODEL.md, device-as-secret posture. */
                int rpc_len = rpc_dispatch(line, response, resp_cap);
                if (rpc_len > 0) {
                    printf("%s\n", response);
                    fflush(stdout);
                }
            } else {
                /* Human console mode */
                int ret;
                esp_err_t err = esp_console_run(line, &ret);
                if (err == ESP_ERR_NOT_FOUND) {
                    printf("Unknown command. Type 'help'.\n");
                } else if (err == ESP_ERR_INVALID_ARG) {
                    /* empty input */
                }
            }
        }
        linenoiseFree(line);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void cli_init(bramble_identity_t* identity) {
    s_identity = identity;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* Install USB Serial JTAG driver and connect it to VFS (stdin/stdout).
     * This is required for linenoise and printf to work over the USB-JTAG
     * console. Only applies to boards with native USB-JTAG (T-Deck Plus,
     * Heltec V4). Heltec V3 uses a CP2102 UART bridge instead. */
    usb_serial_jtag_driver_config_t usj_cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 1024,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj_cfg));
    esp_vfs_usb_serial_jtag_use_driver();
#endif

    esp_console_config_t console_config = {
        .max_cmdline_args = 16,
        .max_cmdline_length = 512,
    };
    esp_console_init(&console_config);

    /* Register commands */
    const esp_console_cmd_t cmds[] = {
        {.command = "peers", .help = "List neighbors", .func = cmd_peers},
        {.command = "status", .help = "Node status", .func = cmd_status},
        {.command = "broadcast", .help = "Send on public channel", .func = cmd_broadcast},
        {.command = "send", .help = "Send to address", .func = cmd_send},
        {.command = "name", .help = "Show or set node name", .func = cmd_name},
        {.command = "wifi", .help = "WiFi management", .func = cmd_wifi},
        {.command = "reboot", .help = "Restart device", .func = cmd_reboot},
        {.command = "help", .help = "Show commands", .func = cmd_help},
    };
    for (int i = 0; i < (int)(sizeof(cmds) / sizeof(cmds[0])); i++) {
        esp_console_cmd_register(&cmds[i]);
    }

    /* Initialize JSON-RPC dispatcher */
    /* RPC init is done in app_main() before cli_init() */

    /* Register UART as notification transport */
    rpc_register_notify_transport(uart_notify_cb, NULL);

    xTaskCreate(cli_task, "cli", 8192, NULL, 1,
                NULL); /* Priority 1: same as main_task, won't starve UI */
    ESP_LOGI(TAG, "CLI initialized");
}
