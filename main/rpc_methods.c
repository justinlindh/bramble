#include <stdio.h> /* sscanf */

#include "rpc_methods.h"
#include "esp_app_desc.h"
#include "util.h"
#include "rpc_dispatcher.h"
#include "rpc_auth.h"
#include "mesh_task.h"
#include "msg_store.h"
#include "airtime_budget.h"
#include "radio.h"
#include "phy_passthrough.h"
#include "tx_gate.h"
#include "freq_plan.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "battery.h"
#include "ota.h"
#include "ota_origin.h"
#include "ota_progress.h"
#include "ota_rollback.h"
#include "ota_url.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_config.h"
#include "display.h"
#include "gps.h"
#include "gps_pref.h"
#include "location.h"
#include "wifi_manager.h"
#include "ws_server.h"
#include "network_key.h"
/* Deep sleep, GPIO wake, esp_wifi and mDNS exist only on the ESP32 targets:
 * not on the POSIX/Linux simulator, and not on the nRF52840 (which has no
 * Wi-Fi at all). The affected RPC handlers degrade on both; see the gates
 * below, which share this condition. */
#if !defined(CONFIG_IDF_TARGET_LINUX) && !defined(BRAMBLE_PLATFORM_NRF)
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "mdns.h"
#endif

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#include "audio.h"
#endif

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
/* statvfs not available in ESP-IDF newlib */

#define BRAMBLE_PROTOCOL_VERSION "0.5.0"

/* NVS namespaces and keys are defined in nvs_keys.h */
#define NVS_NAMESPACE NVS_NS_BRAMBLE

static const char* TAG = "rpc_methods";
static bramble_identity_t* s_identity;

#define LOCATION_SOURCE_KEY "source"
/* LOCATION_CONTACT_RULE_PREFIX, LOCATION_CHANNEL_RULE_PREFIX and the rule
   string codec come from location.h: the send path reads exactly the keys
   this file writes, so they share one definition rather than two that agree
   by inspection. */

static const char* bramble_hardware(void) {
    const bramble_board_config_t* board = board_get_config();
    if (board && board->short_name && board->short_name[0] != '\0') {
        return board->short_name;
    }
    return "unknown";
}

/* Lowercase-hex encode n bytes into out, which must hold 2*n + 1 bytes (the
 * trailing NUL included). Used by the RPC handlers that surface a pubkey,
 * network key, or 4-byte fingerprint as a hex string. */
static void bytes_to_hex(const uint8_t* in, size_t n, char* out) {
    for (size_t i = 0; i < n; i++) {
        snprintf(out + i * 2, 3, "%02x", in[i]);
    }
    out[2 * n] = '\0';
}

typedef struct __attribute__((packed)) {
    int32_t latitude_e7;
    int32_t longitude_e7;
    int16_t altitude_m;
    uint8_t accuracy_m;
    uint8_t speed_kmh;
    uint8_t heading_deg2;
    uint32_t timestamp;
    uint32_t received_ms;
    uint8_t tier;
} persisted_peer_location_t;

/* ── Query handlers (pre-existing) ─────────────────────────────────── */

/*
 * Snapshot scratch policy for the query handlers below.
 *
 * rpc_dispatch() has no serialization of its own and is entered concurrently
 * from three tasks: the WebSocket httpd task, the BLE RPC task and the CLI
 * task. Any handler scratch that outlives the call frame is therefore shared
 * mutable state, and two overlapping getNeighbors calls used to stomp the same
 * ~1.5 KB buffer mid-iteration, so one client got a torn neighbor table.
 *
 * These snapshots (mesh_shared_state_t ~1.5 KB, routing_table_t ~1.8 KB) are
 * per-call heap instead: the buffer is private to the call by construction,
 * and the RPC path is already heap-bound (cJSON builds and prints every
 * response on the heap), so one more short-lived allocation
 * per call is noise next to what the dispatcher already does. A
 * dispatcher-wide mutex was the alternative and was rejected: it would couple
 * the latency of three independent transports, and handlers are not uniformly
 * short (bramble.screenshot blocks up to 3 s waiting on the UI task,
 * bramble.sleep delays 500 ms and then deep-sleeps without returning, several
 * setters block on NVS flash writes), so a slow call on one transport would
 * stall an unrelated getStatus on another.
 *
 * Allocation failure returns RPC_ERR_INTERNAL, which is the honest answer: a
 * node too low on heap to snapshot its own state cannot report that state.
 */

/* bramble.getStatus */
static int handle_get_status(const cJSON* params, cJSON* result) {
    (void)params;
    char buf[12];
    mesh_shared_state_t* st = calloc(1, sizeof(*st));
    if (!st) {
        ESP_LOGE(TAG, "getStatus: out of memory for state snapshot");
        return RPC_ERR_INTERNAL;
    }
    mesh_get_state(st);

    cJSON_AddStringToObject(result, "address", addr_hex(s_identity->address, buf, sizeof(buf)));
    cJSON_AddStringToObject(result, "firmware_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(result, "protocol_version", BRAMBLE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(result, "hardware", bramble_hardware());
    cJSON_AddBoolToObject(result, "radio_ok", st->radio_ok);
    cJSON_AddNumberToObject(result, "peers", st->neighbors.count);
    cJSON_AddNumberToObject(result, "beacon_tx", st->beacon_tx_count);
    cJSON_AddNumberToObject(result, "beacon_rx", st->beacon_rx_count);
    cJSON_AddNumberToObject(result, "packets_tx", st->packets_tx);
    cJSON_AddNumberToObject(result, "packets_rx", st->packets_rx);
    free(st);
    cJSON_AddNumberToObject(result, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(result, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(result, "battery_mv", battery_read_mv());
    cJSON_AddNumberToObject(result, "battery_pct", battery_read_pct());
    cJSON_AddBoolToObject(result, "gps_available", board_has_cap(BOARD_CAP_GPS));
    cJSON_AddBoolToObject(result, "gps_enabled", gps_pref_get());
    cJSON_AddBoolToObject(result, "supports_delivery_event_sync",
                          mesh_supports_delivery_event_sync());

    /* Per-node identity Phase 4 diagnostics: verified-pin count plus the
     * impersonation-signal counters. Additive response fields (no new
     * method); mirrored in api/openapi.yaml's StatusResponse. */
    uint32_t id_pins = 0, id_conflicts = 0, id_sig_failures = 0, id_addr_mismatches = 0;
    uint32_t id_unendorsed = 0, id_expired = 0;
    mesh_get_identity_pin_stats(&id_pins, &id_conflicts, &id_sig_failures, &id_addr_mismatches,
                                &id_unendorsed, &id_expired);
    cJSON_AddNumberToObject(result, "identity_pins", id_pins);
    cJSON_AddNumberToObject(result, "identity_conflicts", id_conflicts);
    cJSON_AddNumberToObject(result, "identity_sig_failures", id_sig_failures);
    cJSON_AddNumberToObject(result, "identity_addr_mismatches", id_addr_mismatches);
    /* Trust-anchor campaign (P2): endorsement-gate rejection counters. Both
     * stay 0 on an unanchored node (the gate never runs). */
    cJSON_AddNumberToObject(result, "identity_unendorsed", id_unendorsed);
    cJSON_AddNumberToObject(result, "identity_expired", id_expired);
    return 0;
}

/* bramble.getDiagnostics */
static int handle_get_diagnostics(const cJSON* params, cJSON* result) {
    bool include_heap_dump = false;
    if (params) {
        const cJSON* dump = cJSON_GetObjectItem(params, "include_heap_dump");
        include_heap_dump = cJSON_IsTrue(dump);
    }

    cJSON_AddNumberToObject(result, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(result, "free_heap", (double)esp_get_free_heap_size());

    cJSON* heap = cJSON_AddObjectToObject(result, "heap");
    cJSON_AddNumberToObject(heap, "internal_free",
                            (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(
        heap, "internal_min_ever_free",
        (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(
        heap, "internal_largest_free_block",
        (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(heap, "dma_free", (double)heap_caps_get_free_size(MALLOC_CAP_DMA));
    cJSON_AddNumberToObject(heap, "dma_largest_free_block",
                            (double)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    cJSON_AddNumberToObject(heap, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(heap, "psram_min_ever_free",
                            (double)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

    static const char* task_names[] = {"main", "mesh",    "ui_gfx",     "wifi",  "sys_evt",
                                       "tiT",  "Tmr Svc", "IDLE0",      "IDLE1", "ipc0",
                                       "ipc1", "ble_rpc", "nimble_host"};
    cJSON* tasks = cJSON_AddArrayToObject(result, "task_stack_hwm");
    for (size_t i = 0; i < (sizeof(task_names) / sizeof(task_names[0])); i++) {
        TaskHandle_t h = xTaskGetHandle(task_names[i]);
        if (!h) {
            continue;
        }

        UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(h);
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "task", task_names[i]);
        cJSON_AddNumberToObject(obj, "hwm_words", (double)hwm_words);
        cJSON_AddNumberToObject(obj, "hwm_bytes", (double)(hwm_words * sizeof(StackType_t)));
        cJSON_AddItemToArray(tasks, obj);
    }

    /* Airtime backpressure counters. Congestion that is absorbed silently is
     * congestion nobody can diagnose from the field, so both the flood relay
     * drops (issue #87) and the PROBE ingress refusals (issue #75) are
     * readable here. */
    cJSON* backpressure = cJSON_AddObjectToObject(result, "backpressure");
    cJSON_AddNumberToObject(backpressure, "flood_relay_drops",
                            (double)mesh_get_flood_relay_drops());

    uint32_t probe_accepted = 0, probe_drop_reply = 0, probe_drop_fwd = 0;
    mesh_get_probe_ingress_stats(&probe_accepted, &probe_drop_reply, &probe_drop_fwd);
    cJSON* probe = cJSON_AddObjectToObject(backpressure, "probe_ingress");
    cJSON_AddNumberToObject(probe, "accepted", (double)probe_accepted);
    cJSON_AddNumberToObject(probe, "dropped_reply", (double)probe_drop_reply);
    cJSON_AddNumberToObject(probe, "dropped_forward", (double)probe_drop_fwd);

    /* GNSS raw-feed diagnostics: byte/line counters and chip banner tell
     * "UART dead" from "flowing but unparseable" on a console-less board. */
    if (board_has_cap(BOARD_CAP_GPS)) {
        gps_debug_t gd;
        gps_get_debug(&gd);
        cJSON_AddNumberToObject(result, "gps_rx_bytes", gd.rx_bytes_total);
        cJSON_AddNumberToObject(result, "gps_rx_lines", gd.rx_lines_total);
        cJSON_AddStringToObject(result, "gps_chip", gd.chip);
        cJSON_AddNumberToObject(result, "gps_rx_overruns", gd.rx_overruns);
        cJSON_AddNumberToObject(result, "gps_rx_errors", gd.rx_errors);
        cJSON_AddNumberToObject(result, "gps_rx_disabled", gd.rx_disabled);
        cJSON_AddNumberToObject(result, "gps_rx_rearm_fail", gd.rx_rearm_fail);
    }

    if (include_heap_dump) {
        ESP_LOGI(
            TAG,
            "bramble.getDiagnostics requested heap_caps_dump(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)");
        heap_caps_dump(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    return 0;
}

/* bramble.getWifiStatus */
static int handle_get_wifi_status(const cJSON* params, cJSON* result) {
    (void)params;

    wifi_status_t status = {0};
    wifi_manager_get_status(&status);

    const char* mode = "off";
    if (status.mode == BRAMBLE_WIFI_STATION) {
        mode = "station";
    } else if (status.mode == BRAMBLE_WIFI_AP) {
        mode = "ap";
    }

    uint8_t mac[6] = {0};
    int clients = 0;
#if !defined(CONFIG_IDF_TARGET_LINUX) && !defined(BRAMBLE_PLATFORM_NRF)
    esp_err_t mac_rc =
        esp_wifi_get_mac(status.mode == BRAMBLE_WIFI_AP ? WIFI_IF_AP : WIFI_IF_STA, mac);

    if (status.mode == BRAMBLE_WIFI_AP) {
        wifi_sta_list_t sta_list = {0};
        if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
            clients = sta_list.num;
        }
    }
#else
    esp_err_t mac_rc = ESP_FAIL; /* no esp_wifi on the simulator: mac stays empty */
#endif

    char mac_str[18] = {0};
    if (mac_rc == ESP_OK) {
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5]);
    }

    cJSON_AddStringToObject(result, "mode", mode);
    cJSON_AddStringToObject(result, "ssid", status.ssid);
    cJSON_AddStringToObject(result, "ip", status.ip_addr);
    cJSON_AddNumberToObject(result, "rssi", status.mode == BRAMBLE_WIFI_STATION ? status.rssi : 0);
    cJSON_AddStringToObject(result, "mac", mac_str);
    cJSON_AddNumberToObject(result, "clients", status.mode == BRAMBLE_WIFI_AP ? clients : 0);

    return 0;
}

/* bramble.setWifiConfig: provision WiFi station credentials over RPC (any
 * transport: serial, WS, BLE), so a first-boot device does not need its
 * on-device UI or the AP-mode captive portal to join a network.
 *
 * password is write-only: it is persisted to the same NVS keys
 * wifi_manager reads at boot, but never echoed back here or by any other
 * RPC (see handle_get_wifi_status and handle_get_config, neither of which
 * reads WiFi credentials out of NVS).
 *
 * There is no live station reconfigure path today (wifi_manager only tries
 * station mode once, at wifi_manager_init, from main's boot sequence), so
 * this mirrors the existing AP-mode captive portal in ws_server.c: persist
 * now, apply requires a reboot. Unlike the captive portal, this method does
 * not reboot the device itself; it reports applied="reboot_required" and
 * leaves the caller to invoke bramble.reboot when it is ready. */
static int handle_set_wifi_config(const cJSON* params, cJSON* result) {
#if defined(CONFIG_IDF_TARGET_LINUX) || defined(BRAMBLE_PLATFORM_NRF)
    (void)params;
    cJSON_AddStringToObject(result, "error", "wifi not supported on this hardware");
    return RPC_ERR_NOT_SUPPORTED;
#else
    if (!params)
        return RPC_ERR_INVALID_PARAMS;

    const char* ssid = cJSON_GetStringValue(cJSON_GetObjectItem(params, "ssid"));
    if (!ssid || strlen(ssid) < 1 || strlen(ssid) > 32)
        return RPC_ERR_INVALID_PARAMS;

    const char* password = "";
    cJSON* pass_item = cJSON_GetObjectItem(params, "password");
    if (pass_item) {
        password = cJSON_GetStringValue(pass_item);
        if (!password || strlen(password) > 64)
            return RPC_ERR_INVALID_PARAMS;
    }

    cJSON* mode_item = cJSON_GetObjectItem(params, "mode");
    if (mode_item) {
        const char* mode = cJSON_GetStringValue(mode_item);
        if (!mode || strcmp(mode, "sta") != 0) {
            cJSON_AddStringToObject(result, "error", "only station mode (\"sta\") is supported");
            return RPC_ERR_NOT_SUPPORTED;
        }
    }

    if (wifi_manager_nvs_set_creds(ssid, password) != 0) {
        cJSON_AddStringToObject(result, "error", "failed to persist wifi credentials");
        return RPC_ERR_INTERNAL;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "applied", "reboot_required");
    return 0;
#endif
}

/* bramble.getIdentity */
static int handle_get_identity(const cJSON* params, cJSON* result) {
    (void)params;
    char buf[12];
    cJSON_AddStringToObject(result, "address", addr_hex(s_identity->address, buf, sizeof(buf)));
    cJSON_AddStringToObject(result, "pubkey_hash",
                            addr_hex(s_identity->pubkey_hash, buf, sizeof(buf)));
    /* Full identity Ed25519 public key (64 lowercase hex). Enrollment needs
     * the whole key to request an anchor endorsement; address/pubkey_hash are
     * only truncated hashes. */
    char ed_hex[2 * BRAMBLE_ED25519_PUBKEY_SIZE + 1];
    bytes_to_hex(s_identity->ed25519_public_key, BRAMBLE_ED25519_PUBKEY_SIZE, ed_hex);
    cJSON_AddStringToObject(result, "ed25519_pub", ed_hex);
    return 0;
}

/* bramble.getPeerVerification, params: {"address":"HEXADDR"}. Wraps the
 * mesh accessor (never touches identity_store directly); an unpinned peer is
 * not an error, it is reported as unverified with an empty SAS. */
static int handle_get_peer_verification(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    const char* addr_str = cJSON_GetStringValue(cJSON_GetObjectItem(params, "address"));
    if (!addr_str)
        return RPC_ERR_INVALID_PARAMS;

    uint32_t addr = (uint32_t)strtoul(addr_str, NULL, 16);
    char sas[8] = {0};
    bool verified = false;
    bool key_changed = false;
    if (!mesh_get_peer_verification(addr, sas, &verified, &key_changed)) {
        cJSON_AddStringToObject(result, "sas", "");
        cJSON_AddBoolToObject(result, "verified", false);
        cJSON_AddBoolToObject(result, "keyChanged", false);
        return 0;
    }

    cJSON_AddStringToObject(result, "sas", sas);
    cJSON_AddBoolToObject(result, "verified", verified);
    cJSON_AddBoolToObject(result, "keyChanged", key_changed);
    return 0;
}

/* bramble.setPeerVerified, params: {"address":"HEXADDR", "verified":bool}.
 * Wraps mesh_set_peer_verified, which records the SAS, persists it, and
 * clears any pending key-change warning on verify. */
static int handle_set_peer_verified(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    const char* addr_str = cJSON_GetStringValue(cJSON_GetObjectItem(params, "address"));
    if (!addr_str)
        return RPC_ERR_INVALID_PARAMS;
    cJSON* verified_j = cJSON_GetObjectItem(params, "verified");
    if (!verified_j || !cJSON_IsBool(verified_j))
        return RPC_ERR_INVALID_PARAMS;

    uint32_t addr = (uint32_t)strtoul(addr_str, NULL, 16);
    bool ok = mesh_set_peer_verified(addr, cJSON_IsTrue(verified_j));
    cJSON_AddBoolToObject(result, "ok", ok);
    return 0;
}

/* bramble.getVersion */
static int handle_get_version(const cJSON* params, cJSON* result) {
    (void)params;
    cJSON_AddStringToObject(result, "firmware_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(result, "protocol_version", BRAMBLE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(result, "hardware", bramble_hardware());
    cJSON_AddBoolToObject(result, "supports_delivery_event_sync",
                          mesh_supports_delivery_event_sync());
    return 0;
}

/* bramble.getDeliveryEvents: params: {sinceEventSeq|since_event_seq, limit?} */
static int handle_get_delivery_events(const cJSON* params, cJSON* result) {
    uint32_t since_seq = 0u;
    uint32_t limit = 256u;

    const cJSON* since = params ? cJSON_GetObjectItem(params, "sinceEventSeq") : NULL;
    if (!since)
        since = params ? cJSON_GetObjectItem(params, "since_event_seq") : NULL;
    if (cJSON_IsNumber(since) && since->valuedouble >= 0) {
        since_seq = (uint32_t)since->valuedouble;
    }

    const cJSON* limit_json = params ? cJSON_GetObjectItem(params, "limit") : NULL;
    if (cJSON_IsNumber(limit_json) && limit_json->valuedouble > 0) {
        limit = (uint32_t)limit_json->valuedouble;
    }
    if (limit > 1024u)
        limit = 1024u;

    size_t out_max = (size_t)((limit < 256u) ? limit : 256u);
    size_t events_bytes = out_max * sizeof(delivery_event_record_t);

    delivery_event_record_t* events = NULL;
    if (events_bytes > 0u) {
        events = heap_caps_malloc(events_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!events) {
            events = malloc(events_bytes);
        }
        if (!events) {
            ESP_LOGE(TAG, "Failed to allocate delivery events buffer (%u bytes)",
                     (unsigned)events_bytes);
            return RPC_ERR_INTERNAL;
        }
    }

    size_t n = mesh_delivery_events_list_since(since_seq, events, out_max);

    cJSON* arr = cJSON_AddArrayToObject(result, "events");
    for (size_t i = 0; i < n; i++) {
        const delivery_event_record_t* e = &events[i];
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "event_seq", e->event_seq);
        cJSON_AddNumberToObject(obj, "timestamp_ms", (double)e->timestamp_s * 1000.0);

        char id_buf[24];
        snprintf(id_buf, sizeof(id_buf), "fw:%" PRIu32, e->event_seq);
        cJSON_AddStringToObject(obj, "event_id", id_buf);

        char msg_buf[12];
        snprintf(msg_buf, sizeof(msg_buf), "%08" PRIX32, e->message_id);

        cJSON* payload = cJSON_CreateObject();
        if (e->event_type == 2u) {
            cJSON_AddStringToObject(obj, "event_type", "broadcast_delivery");
            cJSON_AddStringToObject(obj, "broadcast_id", msg_buf);
            cJSON_AddNumberToObject(payload, "addr", e->recipient_addr);
            cJSON_AddStringToObject(payload, "status", "delivered");
            cJSON_AddNumberToObject(payload, "hopCount", e->route_len);
            cJSON_AddNumberToObject(payload, "deliveredAtMs", (double)e->timestamp_s * 1000.0);
        } else {
            cJSON_AddStringToObject(obj, "event_type", "ack");
            cJSON_AddStringToObject(obj, "packet_id", msg_buf);
            cJSON_AddStringToObject(payload, "status", "delivered");

            cJSON* path = cJSON_AddArrayToObject(payload, "relayPath");
            for (uint8_t h = 0; h < e->route_len && h < DELIVERY_EVENT_ROUTE_MAX_HOPS; h++) {
                cJSON* hop = cJSON_CreateObject();
                char hop_buf[12];
                snprintf(hop_buf, sizeof(hop_buf), "%08" PRIX32, e->route_hops[h]);
                cJSON_AddStringToObject(hop, "addr", hop_buf);
                cJSON_AddNumberToObject(hop, "rssi", 0);
                cJSON_AddItemToArray(path, hop);
            }
        }

        cJSON_AddItemToObject(obj, "payload", payload);
        cJSON_AddItemToArray(arr, obj);
    }

    free(events);

    cJSON_AddNumberToObject(result, "latest_event_seq", mesh_delivery_events_latest_seq());
    return 0;
}

/* bramble.getNeighbors */
static int handle_get_neighbors(const cJSON* params, cJSON* result) {
    (void)params;
    mesh_shared_state_t* st = calloc(1, sizeof(*st));
    if (!st) {
        ESP_LOGE(TAG, "getNeighbors: out of memory for state snapshot");
        return RPC_ERR_INTERNAL;
    }
    mesh_get_state(st);

    cJSON* arr = cJSON_AddArrayToObject(result, "neighbors");
    char buf[12];

    for (int i = 0; i < st->neighbors.count; i++) {
        const neighbor_entry_t* n = &st->neighbors.entries[i];
        if (n->addr == 0)
            continue;

        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "address", addr_hex(n->addr, buf, sizeof(buf)));
        cJSON_AddNumberToObject(obj, "rssi", n->rssi);
        cJSON_AddNumberToObject(obj, "snr", n->snr);
        cJSON_AddNumberToObject(obj, "deliveryRate", n->delivery_rate);
        cJSON_AddNumberToObject(obj, "airtimeRemaining", n->airtime_remaining);
        /* Return time since last heard (ms ago), not absolute timestamp */
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        uint32_t ago = (now > n->last_heard) ? (now - n->last_heard) : 0;
        cJSON_AddNumberToObject(obj, "last_seen_ms", ago);
        if (n->name[0] != '\0') {
            cJSON_AddStringToObject(obj, "name", n->name);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    free(st);
    return 0;
}

/* bramble.getRoutes */
static int handle_get_routes(const cJSON* params, cJSON* result) {
    (void)params;
    routing_table_t* routes = calloc(1, sizeof(*routes));
    if (!routes) {
        ESP_LOGE(TAG, "getRoutes: out of memory for routing-table snapshot");
        return RPC_ERR_INTERNAL;
    }
    mesh_get_routes(routes);

    cJSON* arr = cJSON_AddArrayToObject(result, "routes");
    char buf[12];
    static const char* state_names[] = {"discovering", "unverified", "active", "stale", "broken"};
    for (int i = 0; i < routes->count; i++) {
        const route_entry_t* r = &routes->entries[i];
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "dest", addr_hex(r->dest_addr, buf, sizeof(buf)));
        cJSON_AddStringToObject(obj, "next_hop", addr_hex(r->next_hop, buf, sizeof(buf)));
        cJSON_AddNumberToObject(obj, "hop_count", r->hop_count);
        cJSON_AddNumberToObject(obj, "metric", r->metric);
        cJSON_AddStringToObject(obj, "state",
                                r->state <= ROUTE_BROKEN ? state_names[r->state] : "unknown");
        cJSON_AddNumberToObject(obj, "use_count", r->use_count);
        cJSON_AddItemToArray(arr, obj);
    }
    free(routes);
    return 0;
}

/* bramble.getAirtime */
static int handle_get_airtime(const cJSON* params, cJSON* result) {
    (void)params;
    mesh_shared_state_t* st = calloc(1, sizeof(*st));
    if (!st) {
        ESP_LOGE(TAG, "getAirtime: out of memory for state snapshot");
        return RPC_ERR_INTERNAL;
    }
    mesh_get_state(st);
    /* Refill before reporting so values are current */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    airtime_budget_refill(&st->airtime, now_ms);
    cJSON_AddNumberToObject(result, "critical_remaining_ms",
                            airtime_budget_remaining(&st->airtime, AIRTIME_TIER_CRITICAL));
    cJSON_AddNumberToObject(result, "normal_remaining_ms",
                            airtime_budget_remaining(&st->airtime, AIRTIME_TIER_NORMAL));
    cJSON_AddNumberToObject(result, "broadcast_remaining_ms",
                            airtime_budget_remaining(&st->airtime, AIRTIME_TIER_BROADCAST));
    cJSON_AddNumberToObject(result, "receipt_remaining_ms",
                            airtime_budget_remaining(&st->airtime, AIRTIME_TIER_RECEIPT));
    cJSON_AddNumberToObject(result, "critical_max_ms", st->airtime.max_ms[AIRTIME_IDX_CRITICAL]);
    cJSON_AddNumberToObject(result, "normal_max_ms", st->airtime.max_ms[AIRTIME_IDX_NORMAL]);
    cJSON_AddNumberToObject(result, "broadcast_max_ms", st->airtime.max_ms[AIRTIME_IDX_BROADCAST]);
    cJSON_AddNumberToObject(result, "receipt_max_ms", st->airtime.max_ms[AIRTIME_IDX_RECEIPT]);
    cJSON_AddNumberToObject(result, "next_refill_ms",
                            airtime_budget_next_refill_ms(&st->airtime, now_ms));
    free(st);
    return 0;
}

/* bramble.ping */
static int handle_ping(const cJSON* params, cJSON* result) {
    (void)params;
    char buf[12];
    cJSON_AddBoolToObject(result, "pong", true);
    cJSON_AddStringToObject(result, "address", addr_hex(s_identity->address, buf, sizeof(buf)));
    cJSON_AddStringToObject(result, "protocol_version", BRAMBLE_PROTOCOL_VERSION);
    return 0;
}

/* ── Action handlers ────────────────────────────────────────────────── */

/* ── Fragmentation constants (aligned with components/fragment) ──────── */
/* Single-packet DATA max: 255 - 52 bytes overhead = 203 bytes plaintext */
#define SINGLE_PACKET_MAX_BYTES 203
/* Fragment payload per packet: 154 bytes (from fragment.h FRAG_MAX_PLAINTEXT) */
#define FRAGMENT_PAYLOAD_BYTES 154
/* Max fragments per message: 4 (from fragment.h FRAG_MAX_FRAGMENTS) */
#define MAX_FRAGMENTS 4
/* True max with fragmentation: 154 * 4 = 616 bytes */
#define FRAGMENTED_MAX_BYTES (FRAGMENT_PAYLOAD_BYTES * MAX_FRAGMENTS)

/* bramble.sendMessage: params: {"dest":"HEXADDR", "text":"...", "channel"?:N} */
static int handle_send_message(const cJSON* params, cJSON* result) {
    const char* dest_str = cJSON_GetStringValue(cJSON_GetObjectItem(params, "dest"));
    const char* text = cJSON_GetStringValue(cJSON_GetObjectItem(params, "text"));
    cJSON* channel_j = cJSON_GetObjectItem(params, "channel");
    if (!dest_str || !text) {
        return RPC_ERR_INVALID_PARAMS;
    }

    size_t text_len = strlen(text);

    /* Enforce true fragmented maximum */
    if (text_len > FRAGMENTED_MAX_BYTES) {
        cJSON_AddStringToObject(result, "error", "message too long");
        cJSON_AddNumberToObject(result, "max_bytes", (double)FRAGMENTED_MAX_BYTES);
        cJSON_AddNumberToObject(result, "actual_bytes", (double)text_len);
        cJSON_AddBoolToObject(result, "fragmented", false);
        return RPC_ERR_INVALID_PARAMS;
    }

    uint32_t dest = (uint32_t)strtoul(dest_str, NULL, 16);
    uint32_t pkt_id = 0;

    if (channel_j && cJSON_IsNumber(channel_j)) {
        int ch = channel_j->valueint;
        if (ch < 0 || ch >= mesh_get_channel_count()) {
            cJSON_AddStringToObject(result, "error", "invalid channel");
            return RPC_ERR_INVALID_PARAMS;
        }

        /* Backward compatibility alias used by webapp channel conversations. */
        if (dest == 0xFFFFFFFE) {
            dest = 0xFFFFFFFF;
        }

        pkt_id = mesh_send_channel(ch, dest, (const uint8_t*)text, text_len);
        if (pkt_id == 0) {
            ESP_LOGW(TAG, "mesh_send_channel ch=%d to %08" PRIX32 " failed", ch, dest);
            cJSON_AddStringToObject(result, "error", "send failed");
            return RPC_ERR_RADIO;
        }
        cJSON_AddNumberToObject(result, "channel", ch);
    } else {
        pkt_id = mesh_send_message(dest, (const uint8_t*)text, text_len);
        if (pkt_id == 0) {
            ESP_LOGW(TAG, "mesh_send_message to %08" PRIX32 " failed", dest);
            cJSON_AddStringToObject(result, "error", "send failed");
            return RPC_ERR_RADIO;
        }
    }

    char pkt_id_str[12];
    snprintf(pkt_id_str, sizeof(pkt_id_str), "%08" PRIX32, pkt_id);
    cJSON_AddStringToObject(result, "packetId", pkt_id_str);
    cJSON_AddStringToObject(result, "status", "sent");

    /* Add fragmentation metadata */
    bool will_fragment = text_len > SINGLE_PACKET_MAX_BYTES;
    cJSON_AddBoolToObject(result, "fragmented", will_fragment);
    if (will_fragment) {
        int frag_count = (int)((text_len + FRAGMENT_PAYLOAD_BYTES - 1) / FRAGMENT_PAYLOAD_BYTES);
        cJSON_AddNumberToObject(result, "fragments_total", frag_count);
    }
    cJSON_AddNumberToObject(result, "max_bytes", (double)FRAGMENTED_MAX_BYTES);
    cJSON_AddNumberToObject(result, "actual_bytes", (double)text_len);

    return 0;
}

/* bramble.sendBroadcast: params: {"text":"..."} */
static int handle_send_broadcast(const cJSON* params, cJSON* result) {
    const char* text = cJSON_GetStringValue(cJSON_GetObjectItem(params, "text"));
    if (!text) {
        return RPC_ERR_INVALID_PARAMS;
    }

    size_t text_len = strlen(text);

    /* Enforce true fragmented maximum */
    if (text_len > FRAGMENTED_MAX_BYTES) {
        cJSON_AddStringToObject(result, "error", "message too long");
        cJSON_AddNumberToObject(result, "max_bytes", (double)FRAGMENTED_MAX_BYTES);
        cJSON_AddNumberToObject(result, "actual_bytes", (double)text_len);
        cJSON_AddBoolToObject(result, "fragmented", false);
        return RPC_ERR_INVALID_PARAMS;
    }

    int rc = mesh_send_broadcast((const uint8_t*)text, text_len);
    if (rc == -2) {
        cJSON_AddStringToObject(result, "error", "rate limited");
        return RPC_ERR_RATE_LIMIT;
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "mesh_send_broadcast failed: %d", rc);
        cJSON_AddStringToObject(result, "error", "send failed");
        return RPC_ERR_RADIO;
    }

    uint32_t broadcast_id = mesh_get_last_broadcast_id();
    if (broadcast_id == 0) {
        return RPC_ERR_RADIO;
    }

    char bcast_id_buf[12];
    snprintf(bcast_id_buf, sizeof(bcast_id_buf), "%08" PRIX32, broadcast_id);
    cJSON_AddStringToObject(result, "broadcast_id", bcast_id_buf);
    cJSON_AddStringToObject(result, "status", "sent");
    cJSON_AddBoolToObject(result, "broadcast", true);
    cJSON_AddNumberToObject(result, "channel", -1);

    /* Add fragmentation metadata */
    bool will_fragment = text_len > SINGLE_PACKET_MAX_BYTES;
    cJSON_AddBoolToObject(result, "fragmented", will_fragment);
    if (will_fragment) {
        int frag_count = (int)((text_len + FRAGMENT_PAYLOAD_BYTES - 1) / FRAGMENT_PAYLOAD_BYTES);
        cJSON_AddNumberToObject(result, "fragments_total", frag_count);
    }
    cJSON_AddNumberToObject(result, "max_bytes", (double)FRAGMENTED_MAX_BYTES);
    cJSON_AddNumberToObject(result, "actual_bytes", (double)text_len);

    return 0;
}

/* bramble.reboot: no params required */
static int handle_reboot(const cJSON* params, cJSON* result) {
    (void)params;
    cJSON_AddBoolToObject(result, "ok", true);
    mesh_reboot_delayed(500);
    return 0;
}

/* bramble.enterDfu: no params. Reboots into the firmware-update bootloader
 * on platforms that have one; the platform hook returns nonzero where the
 * concept does not exist (ESP32, which updates over OTA instead). On the
 * nRF UF2 boards this is the only reflash path that needs no physical
 * button: the T1000-E is consoleless and its DFU button gesture is a
 * timing-sensitive cable dance. */
__attribute__((weak)) int bramble_platform_enter_dfu(void) { return -1; }

static int handle_enter_dfu(const cJSON* params, cJSON* result) {
    (void)params;
    if (bramble_platform_enter_dfu() != 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "DFU mode not supported on this platform");
        return 0;
    }
    /* The hook schedules the reset with a delay so this response can flush
     * to the client first. */
    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

/* bramble.sendProbe: no params. Broadcasts a probe (mesh_send_probe takes no
 * destination); responses arrive as bramble.onProbeResult notifications,
 * followed by bramble.onProbeComplete. */
static int handle_send_probe(const cJSON* params, cJSON* result) {
    (void)params;
    uint32_t probe_id = mesh_send_probe();
    if (probe_id == 0) {
        return RPC_ERR_INTERNAL;
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "%08" PRIX32, probe_id);
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "probe_id", buf);
    cJSON_AddNumberToObject(result, "ack_window", 5);
    cJSON_AddNumberToObject(result, "rounds_total", 3);
    return 0;
}

/* Commits pending nvs_set_* writes (only if every prior one succeeded) and
 * closes the handle. Returns the resulting esp_err_t. Shared by the RPC
 * handlers below that accumulate esp_err_t across a chain of nvs_set_*
 * calls before persisting. */
static esp_err_t rpc_nvs_commit_close(nvs_handle_t nvs, esp_err_t err) {
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* Logs an NVS persist failure and, when set_result is true, reports it to the
 * client as {"ok":false,"error":client_msg}. Returns rpc_rc so callers can
 * `return rpc_report_persist_failure(...)` directly. Centralizes the log and
 * report shape repeated across the RPC handlers that persist to NVS. */
static int rpc_report_persist_failure(cJSON* result, const char* what, esp_err_t err,
                                      bool set_result, const char* client_msg, int rpc_rc) {
    ESP_LOGE(TAG, "%s: %d", what, err);
    if (set_result) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", client_msg);
    }
    return rpc_rc;
}

/* bramble.setRadio: a params object is required, every field in it is
 * optional (omitted fields keep their current value):
 * {"frequency_mhz":915.0, "sf":9, "bw_hz":125000, "tx_power_dbm":17,
 * "coding_rate":5} */
static int handle_set_radio(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;

    /* Get current config as base */
    radio_config_t cfg;
    radio_get_config(&cfg);

    /* Apply any provided fields */
    cJSON* freq = cJSON_GetObjectItem(params, "frequency_mhz");
    cJSON* sf = cJSON_GetObjectItem(params, "sf");
    cJSON* bw = cJSON_GetObjectItem(params, "bw_hz");
    cJSON* txp = cJSON_GetObjectItem(params, "tx_power_dbm");
    cJSON* cr = cJSON_GetObjectItem(params, "coding_rate");

    if (freq && cJSON_IsNumber(freq))
        cfg.frequency_mhz = (float)freq->valuedouble;
    if (sf && cJSON_IsNumber(sf))
        cfg.sf = (uint8_t)sf->valueint;
    if (bw && cJSON_IsNumber(bw))
        cfg.bw_hz = (uint32_t)bw->valuedouble;
    if (txp && cJSON_IsNumber(txp))
        cfg.tx_power = (int8_t)txp->valueint;
    if (cr && cJSON_IsNumber(cr))
        cfg.coding_rate = (uint8_t)cr->valueint;

    /* Validate against freq plan */
    const bramble_freq_plan_t* plan = freq_plan_get_default();
    if (!freq_plan_valid_freq(plan, cfg.frequency_mhz)) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "frequency out of region bounds");
        return 0;
    }
    cfg.tx_power = freq_plan_clamp_power(plan, cfg.tx_power);
    if (cfg.sf < 6 || cfg.sf > 12) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "SF must be 6-12");
        return 0;
    }
    if (cfg.bw_hz != 125000 && cfg.bw_hz != 250000 && cfg.bw_hz != 500000) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "BW must be 125000, 250000, or 500000");
        return 0;
    }

    /* Apply */
    int rc = radio_reconfigure(&cfg);
    if (rc != 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "radio reconfigure failed");
        return 0;
    }

    /* Persist to NVS */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_RADIO, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        /* Store as integers to avoid float NVS issues */
        err = nvs_set_u32(nvs, "freq_khz", (uint32_t)(cfg.frequency_mhz * 1000));
        if (err == ESP_OK)
            err = nvs_set_u8(nvs, "sf", cfg.sf);
        if (err == ESP_OK)
            err = nvs_set_u32(nvs, "bw_hz", cfg.bw_hz);
        if (err == ESP_OK)
            err = nvs_set_i8(nvs, "tx_power", cfg.tx_power);
        if (err == ESP_OK)
            err = nvs_set_u8(nvs, "cr", cfg.coding_rate);
        err = rpc_nvs_commit_close(nvs, err);
    }

    if (err != ESP_OK) {
        /* Radio was already reconfigured live above; only persistence
         * failed, so the change is real until the next reboot. */
        return rpc_report_persist_failure(result, "radio config persist failed", err, true,
                                          "failed to persist; setting lost on reboot", 0);
    }

    ESP_LOGI(TAG, "Radio reconfigured: %.1f MHz SF%u BW%" PRIu32 " TX %ddBm", cfg.frequency_mhz,
             cfg.sf, cfg.bw_hz, cfg.tx_power);

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "frequency_mhz", cfg.frequency_mhz);
    cJSON_AddNumberToObject(result, "sf", cfg.sf);
    cJSON_AddNumberToObject(result, "bw_hz", cfg.bw_hz);
    cJSON_AddNumberToObject(result, "tx_power_dbm", cfg.tx_power);
    cJSON_AddNumberToObject(result, "coding_rate", cfg.coding_rate);
    return 0;
}

/* bramble.setNodeName: params: {"name":"..."}, persists to NVS */
static int handle_set_node_name(const cJSON* params, cJSON* result) {
    const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(params, "name"));
    if (!name || strlen(name) == 0 || strlen(name) > BRAMBLE_NODE_NAME_MAX) {
        return RPC_ERR_INVALID_PARAMS;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %d", err);
        cJSON_AddStringToObject(result, "error", "nvs_open failed");
        return RPC_ERR_INTERNAL;
    }

    err = nvs_set_str(nvs, NVS_KEY_NODE_NAME, name);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str failed: %d", err);
        cJSON_AddStringToObject(result, "error", "nvs write failed");
        return RPC_ERR_INTERNAL;
    }

    /* Update runtime name so beacons immediately reflect the change */
    mesh_set_node_name(name);

    /* Best-effort: reflect the new name in the mDNS TXT record so discovery
     * shows it without a reboot. Fails harmlessly when mDNS is not running
     * (AP mode / WiFi off). */
#if !defined(CONFIG_IDF_TARGET_LINUX) && !defined(BRAMBLE_PLATFORM_NRF)
    (void)mdns_service_txt_item_set("_bramble", "_tcp", "name", name);
#endif

    ESP_LOGI(TAG, "Node name set to: %s", name);
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "name", name);
    return 0;
}

static int rpc_set_auth_token(const cJSON* params, cJSON* result) {
    const cJSON* token_j = cJSON_GetObjectItem(params, "token");
    if (!token_j || !cJSON_IsString(token_j)) {
        return RPC_ERR_INVALID_PARAMS;
    }
    const char* val = token_j->valuestring;
    if (strlen(val) >= 128) {
        return RPC_ERR_INVALID_PARAMS;
    }
    /* Entropy floor (SEC-H3): a guessable token is worse than knowing you
     * have none. Empty stays legal as the explicit opt-out. */
    if (!rpc_auth_token_len_ok(strlen(val))) {
        return RPC_ERR_INVALID_PARAMS;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %d", err);
        return RPC_ERR_INTERNAL;
    }
    if (val[0] == '\0') {
        /* Empty token = explicit opt-out: persist the auth_off flag so the
         * first-boot generator does not re-create a token on next boot.
         * Reaching this handler already required auth, so only a token
         * holder (or serial/physical access) can open the device up. */
        err = nvs_set_u8(h, NVS_KEY_AUTH_OFF, 1);
        if (err == ESP_OK) {
            err = nvs_erase_key(h, NVS_KEY_AUTH_TOKEN);
            if (err == ESP_ERR_NVS_NOT_FOUND)
                err = ESP_OK; /* no existing token to erase is not a failure */
        }
        if (err == ESP_OK)
            ESP_LOGW(TAG, "RPC auth explicitly disabled via setAuthToken; device is open access");
    } else {
        /* Set token and clear the opt-out flag */
        err = nvs_set_u8(h, NVS_KEY_AUTH_OFF, 0);
        if (err == ESP_OK)
            err = nvs_set_str(h, NVS_KEY_AUTH_TOKEN, val);
    }
    err = rpc_nvs_commit_close(h, err);

    if (err != ESP_OK) {
        return rpc_report_persist_failure(result, "auth token persist failed", err, false, NULL,
                                          RPC_ERR_INTERNAL);
    }

    ws_server_load_token(); /* reload immediately */
    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* bramble.setNetworkKey: params {"key": "<64 lowercase/uppercase hex chars>"}.
 * Provisions the control-plane network key (PART 3, STAGED: see
 * network_key.h. This does NOT by itself close SEC-H1, SEC-H2, NEW-SEC-4,
 * or NEW-SEC-8; those stay open until real per-fleet key distribution
 * lands). Authenticated callers only: this method is not in rpc_auth's
 * unauth allowlist, so an unauthenticated caller cannot reach it, exactly
 * mirroring setAuthToken above. Persists to NVS (NVS_NS_NETKEY) so the
 * provisioned key survives reboot, mirroring the identity/nonce NVS
 * pattern. Distribution UX, how an operator actually gets a key onto a
 * fleet of devices, is out of scope here: an open question for the
 * provisioning workstream, not solved by this RPC. */
static int rpc_set_network_key(const cJSON* params, cJSON* result) {
    const cJSON* key_j = cJSON_GetObjectItem(params, "key");
    if (!key_j || !cJSON_IsString(key_j)) {
        return RPC_ERR_INVALID_PARAMS;
    }
    const char* hex = key_j->valuestring;
    if (strlen(hex) != 64) {
        return RPC_ERR_INVALID_PARAMS;
    }
    uint8_t key[32];
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return RPC_ERR_INVALID_PARAMS;
        }
        key[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Mandatory-provisioning (Task 2): single source of truth. The network_key
     * component persists to NVS (NVS_NS_NETKEY) on set, so this no longer
     * hand-rolls its own NVS write; that removes the double-write and the risk
     * of the RPC and the component disagreeing on the stored key. */
    network_key_set_provisioned(key);
    mesh_rederive_beacon_key(); /* beacons pick up the new key live, no reboot */
    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

/* bramble.getNetworkKeyStatus: params none. Result:
 *   {"provisioned": bool, "fingerprint": "<8 lowercase hex>"}
 * Reports whether a real per-fleet key is set and a one-way fingerprint
 * (SHA256(key)[0:4]) so an operator can confirm a fleet shares one key
 * WITHOUT the key ever being read back. Authenticated: registered normally,
 * so it is not in rpc_auth's unauth allowlist. This does NOT close SEC-H1/
 * H2/NEW-SEC-4/NEW-SEC-8; it is provisioning observability, not closure. */
static int handle_get_network_key_status(const cJSON* params, cJSON* result) {
    (void)params;
    cJSON_AddBoolToObject(result, "provisioned", network_key_is_provisioned() ? true : false);
    uint8_t fp[4];
    network_key_fingerprint(fp);
    char hex[9];
    bytes_to_hex(fp, 4, hex);
    cJSON_AddStringToObject(result, "fingerprint", hex);
    return 0;
}

/* bramble.generateNetworkKey: params none. Result:
 *   {"key": "<64 lowercase hex>", "fingerprint": "<8 lowercase hex>"}
 * Mints a fresh entropy-gated 32-byte network key via
 * network_key_generate_provision (mandatory-provisioning Task 1), which
 * provisions THIS node (in memory + NVS) atomically and copies the key out.
 * This makes the local node the fleet "founder": the operator then copies the
 * displayed key to the other nodes (via setNetworkKey / the QR share).
 *
 * Returning the RAW key here is deliberate and acceptable: this is the
 * operator's own LOCAL control channel (WiFi/BLE/serial to their own node),
 * the identical trust boundary setNetworkKey already relies on to ACCEPT a
 * raw key over the same channel. The key is never broadcast and never logged;
 * the local stack copy is wiped before returning. Authenticated callers only
 * (registered normally, so not in rpc_auth's unauth allowlist).
 *
 * On entropy failure network_key_generate_provision provisions NOTHING and
 * returns nonzero; we surface a JSON-RPC internal error and leave the prior
 * provisioning state untouched (fail closed). */
static int handle_generate_network_key(const cJSON* params, cJSON* result) {
    (void)params;
    uint8_t key[32];
    if (network_key_generate_provision(key) != 0) {
        return RPC_ERR_INTERNAL;
    }

    char key_hex[65];
    bytes_to_hex(key, 32, key_hex);
    memset(key, 0, sizeof(key)); /* wipe local copy; never log the key */
    cJSON_AddStringToObject(result, "key", key_hex);

    mesh_rederive_beacon_key(); /* beacons pick up the new key live, no reboot */

    uint8_t fp[4];
    network_key_fingerprint(fp);
    char fp_hex[9];
    bytes_to_hex(fp, 4, fp_hex);
    cJSON_AddStringToObject(result, "fingerprint", fp_hex);
    return 0;
}

/* bramble.setAnchor: params {"anchor_pubkey": "<64 hex chars>"}.
 * Provisions the fleet trust-anchor PUBLIC key (trust-anchor campaign, P0).
 * The anchor holder is an offline operator client; the device only ever holds
 * the anchor PUBLIC key, never the private key, and never signs endorsements.
 * Persisted via identity_anchor_set (NVS on device). P0 is inert: nothing
 * reads the anchor for a trust decision yet (later phases pin endorsed-only).
 * Authenticated callers only (registered normally, so not in rpc_auth's
 * unauth allowlist), mirroring setNetworkKey. */
static int rpc_set_anchor(const cJSON* params, cJSON* result) {
    const cJSON* key_j = cJSON_GetObjectItem(params, "anchor_pubkey");
    if (!key_j || !cJSON_IsString(key_j)) {
        return RPC_ERR_INVALID_PARAMS;
    }
    const char* hex = key_j->valuestring;
    if (strlen(hex) != 64) {
        return RPC_ERR_INVALID_PARAMS;
    }
    uint8_t pub[BRAMBLE_ED25519_PUBKEY_SIZE];
    for (int i = 0; i < BRAMBLE_ED25519_PUBKEY_SIZE; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return RPC_ERR_INVALID_PARAMS;
        }
        pub[i] = (uint8_t)((hi << 4) | lo);
    }
    identity_anchor_set(pub);
    /* Trust-anchor campaign (P2): push the anchor into the live pin store so it
     * pins only endorsed identities immediately, without waiting for a reboot.
     * The boot path also loads it, so a reboot is never required for
     * correctness; this just makes runtime provisioning take effect at once. */
    mesh_set_pin_anchor(pub);
    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

/* bramble.getAnchorStatus: params none. Result:
 *   {"anchored": bool, "anchor_fingerprint": "<8 lowercase hex>"}
 * Reports whether a fleet trust anchor is provisioned and, when it is, a
 * one-way fingerprint (SHA256(anchor_pub)[0:4]) so an operator can confirm a
 * fleet shares one anchor without the key being echoed. The fingerprint field
 * is present only when anchored. The "endorsed" flag reports whether THIS node
 * holds its own endorsement cert (trust-anchor campaign, P1) so an operator
 * can see enrollment state. Mirrors getNetworkKeyStatus. */
static int handle_get_anchor_status(const cJSON* params, cJSON* result) {
    (void)params;
    bool anchored = identity_anchor_is_set();
    cJSON_AddBoolToObject(result, "anchored", anchored);
    if (anchored) {
        uint8_t fp[4];
        identity_anchor_fingerprint(fp);
        char hex[9];
        bytes_to_hex(fp, 4, hex);
        cJSON_AddStringToObject(result, "anchor_fingerprint", hex);
    }
    /* endorsed = we hold a cert that ACTUALLY verifies against the CURRENT
     * anchor + this node's own identity key, not mere presence. After an
     * anchor rotation (setAnchor A2 while a cert signed by A1 is still stored)
     * the old cert is dead, so this must report false. P3's gates and P4's
     * webapp read this as live enrollment state. The stored cert is NOT
     * cleared here: idempotent re-provisioning of the same anchor must keep
     * working; only what we REPORT changes. */
    bool endorsed = false;
    if (anchored && identity_endorsement_is_set()) {
        uint64_t not_after;
        uint8_t sig[BRAMBLE_ED25519_SIG_SIZE];
        uint8_t anchor_pub[BRAMBLE_ED25519_PUBKEY_SIZE];
        if (identity_endorsement_get(&not_after, sig) == 0 &&
            identity_anchor_get(anchor_pub) == 0) {
            endorsed = identity_endorsement_verify(anchor_pub, s_identity->ed25519_public_key,
                                                   not_after, sig);
        }
    }
    cJSON_AddBoolToObject(result, "endorsed", endorsed);
    return 0;
}

/* bramble.setEndorsement: params
 *   {"not_after": "<16 hex chars, big-endian uint64>",
 *    "endorsement_sig": "<128 hex chars>"}.
 * Provisions THIS node's own endorsement cert (trust-anchor campaign, P1):
 * the anchor's signature vouching for this node's Ed25519 identity key.
 * not_after is a HEX STRING, not a JSON number, because UINT64_MAX (the
 * permanent sentinel, "ffffffffffffffff") does not survive JSON double
 * precision. The device never SIGNS an endorsement; it only accepts one the
 * anchor already signed, and verifies it against this node's own identity key
 * and the provisioned anchor before persisting. Rejected when no anchor is
 * provisioned, when not_after == 0 (the "no cert" sentinel), when either field
 * is malformed, or when the signature does not verify. On success the cert is
 * persisted and a fresh attestation is triggered so the fleet learns it
 * promptly. Authenticated callers only (registered normally), mirroring
 * setAnchor. */
static int rpc_set_endorsement(const cJSON* params, cJSON* result) {
    /* No anchor provisioned: there is nothing to verify the cert against, so
     * a cert cannot be meaningfully accepted. Reject before parsing. */
    if (!identity_anchor_is_set()) {
        return RPC_ERR_INVALID_PARAMS;
    }

    const cJSON* na_j = cJSON_GetObjectItem(params, "not_after");
    const cJSON* sig_j = cJSON_GetObjectItem(params, "endorsement_sig");
    if (!na_j || !cJSON_IsString(na_j) || !sig_j || !cJSON_IsString(sig_j)) {
        return RPC_ERR_INVALID_PARAMS;
    }
    const char* na_hex = na_j->valuestring;
    const char* sig_hex = sig_j->valuestring;
    if (strlen(na_hex) != 16 || strlen(sig_hex) != 128) {
        return RPC_ERR_INVALID_PARAMS;
    }

    /* not_after: 16 hex chars, big-endian uint64. */
    uint64_t not_after = 0;
    for (int i = 0; i < 16; i++) {
        int nib = hex_nibble(na_hex[i]);
        if (nib < 0) {
            return RPC_ERR_INVALID_PARAMS;
        }
        not_after = (not_after << 4) | (uint64_t)nib;
    }
    /* 0 is the "no cert present" sentinel; refuse to store it as a cert. */
    if (not_after == IDENTITY_ENDORSEMENT_NOT_AFTER_NONE) {
        return RPC_ERR_INVALID_PARAMS;
    }

    uint8_t sig[BRAMBLE_ED25519_SIG_SIZE];
    for (int i = 0; i < BRAMBLE_ED25519_SIG_SIZE; i++) {
        int hi = hex_nibble(sig_hex[i * 2]);
        int lo = hex_nibble(sig_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return RPC_ERR_INVALID_PARAMS;
        }
        sig[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Verify against this node's own identity key + the provisioned anchor
     * before persisting: the device stores only a cert the anchor genuinely
     * signed over this exact key and window. */
    uint8_t anchor_pub[BRAMBLE_ED25519_PUBKEY_SIZE];
    if (identity_anchor_get(anchor_pub) != 0) {
        return RPC_ERR_INVALID_PARAMS;
    }
    if (!identity_endorsement_verify(anchor_pub, s_identity->ed25519_public_key, not_after, sig)) {
        return RPC_ERR_INVALID_PARAMS;
    }

    identity_endorsement_set(not_after, sig);
    /* Re-announce so the fleet learns the cert promptly (mirrors the attest-on
     * -address/key-change hook in mesh_task.c). */
    mesh_trigger_attestation();
    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

/* bramble.setAllowedOrigins: params {"origins":["https://app.example.com", ...]}
 * Persists the extra WS Origin allowlist (authenticated callers only; the
 * dispatcher's unauth allowlist never includes this method). Origins are
 * stored as a comma-separated list, so entries must not contain commas or
 * spaces; full origins (scheme://host[:port]) never do. */
static int rpc_set_allowed_origins(const cJSON* params, cJSON* result) {
    const cJSON* origins_j = cJSON_GetObjectItem(params, "origins");
    if (!origins_j || !cJSON_IsArray(origins_j)) {
        return RPC_ERR_INVALID_PARAMS;
    }

    char joined[256] = {0};
    size_t used = 0;
    const cJSON* entry = NULL;
    cJSON_ArrayForEach(entry, origins_j) {
        if (!cJSON_IsString(entry) || entry->valuestring[0] == '\0') {
            return RPC_ERR_INVALID_PARAMS;
        }
        const char* o = entry->valuestring;
        size_t olen = strlen(o);
        if (strchr(o, ',') || strchr(o, ' ')) {
            return RPC_ERR_INVALID_PARAMS;
        }
        /* Entries are full origins ("scheme://host[:port]") or the
         * literal "null" opt-in for sandboxed/file pages. */
        if (strcmp(o, "null") != 0 && strstr(o, "://") == NULL) {
            return RPC_ERR_INVALID_PARAMS;
        }
        if (used + olen + (used ? 1 : 0) >= sizeof(joined)) {
            return RPC_ERR_INVALID_PARAMS; /* list too long for NVS slot */
        }
        if (used) {
            joined[used++] = ',';
        }
        memcpy(joined + used, o, olen);
        used += olen;
    }
    joined[used] = '\0';

    nvs_handle_t h;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &h) != ESP_OK) {
        return RPC_ERR_INTERNAL;
    }
    esp_err_t err;
    if (used == 0) {
        err = nvs_erase_key(h, NVS_KEY_WS_ORIGINS);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(h, NVS_KEY_WS_ORIGINS, joined);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return RPC_ERR_INTERNAL;
    }

    ws_server_load_origins(); /* apply immediately */
    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

static int rpc_get_allowed_origins(const cJSON* params, cJSON* result) {
    (void)params;
    cJSON* arr = cJSON_AddArrayToObject(result, "origins");
    const char* p = ws_server_get_extra_origins();
    while (p && *p != '\0') {
        while (*p == ',') {
            p++;
        }
        size_t len = 0;
        while (p[len] != '\0' && p[len] != ',') {
            len++;
        }
        if (len > 0) {
            char buf[256];
            if (len >= sizeof(buf)) {
                len = sizeof(buf) - 1;
            }
            memcpy(buf, p, len);
            buf[len] = '\0';
            cJSON_AddItemToArray(arr, cJSON_CreateString(buf));
        }
        p += len;
    }
    return 0;
}

static int rpc_get_auth_token(const cJSON* params, cJSON* result) {
    (void)params;
    const char* token = ws_server_get_token();
    cJSON_AddStringToObject(result, "token", (token && token[0]) ? token : "");
    cJSON_AddBoolToObject(result, "enabled", token && token[0] != '\0');
    return 0;
}

/* bramble.addChannel: params {"name":"...", "psk":"passphrase"} */
static int handle_add_channel(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(params, "name"));
    const char* psk = cJSON_GetStringValue(cJSON_GetObjectItem(params, "psk"));
    if (!name || strlen(name) == 0 || strlen(name) > 19) {
        return RPC_ERR_INVALID_PARAMS;
    }

    const uint8_t* psk_ptr = NULL;
    size_t psk_len = 0;
    if (psk && psk[0] != '\0') {
        psk_ptr = (const uint8_t*)psk;
        psk_len = strlen(psk);
    }

    int idx = mesh_add_channel(name, psk_ptr, psk_len);
    if (idx < 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "channel limit reached");
        return 0;
    }

    /* Persistence is now handled in mesh_add_channel (Phase 1) */

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "index", idx);
    cJSON_AddStringToObject(result, "name", name);
    return 0;
}

static int handle_remove_channel(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    cJSON* idx_j = cJSON_GetObjectItem(params, "index");
    if (!idx_j || !cJSON_IsNumber(idx_j))
        return RPC_ERR_INVALID_PARAMS;
    int index = idx_j->valueint;

    if (index == 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "cannot remove public channel");
        return 0;
    }

    int rc = mesh_remove_channel(index);
    if (rc != 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "invalid channel index");
        return 0;
    }

    /* Persistence is now handled in mesh_remove_channel (Phase 1) */

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "channels", mesh_get_channel_count());
    return 0;
}

static int handle_set_default_channel(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    cJSON* idx_j = cJSON_GetObjectItem(params, "index");
    if (!idx_j || !cJSON_IsNumber(idx_j))
        return RPC_ERR_INVALID_PARAMS;

    int rc = mesh_set_default_channel(idx_j->valueint);
    if (rc != 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "invalid channel index");
        return 0;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

static int handle_set_mailbox(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    cJSON* enabled = cJSON_GetObjectItem(params, "enabled");
    if (!enabled || !cJSON_IsBool(enabled))
        return RPC_ERR_INVALID_PARAMS;

    /* Persist to NVS */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_MAILBOX, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "enabled", cJSON_IsTrue(enabled) ? 1 : 0);
        err = rpc_nvs_commit_close(nvs, err);
    }

    if (err != ESP_OK) {
        return rpc_report_persist_failure(result, "mailbox persist failed", err, true,
                                          "nvs write failed", 0);
    }

    bool en = cJSON_IsTrue(enabled);
    mesh_set_mailbox(en);

    ESP_LOGI("rpc", "Mailbox %s", en ? "enabled" : "disabled");
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddBoolToObject(result, "enabled", en);
    return 0;
}

/* Flooding F1 Task 1: bramble.setFloodTransport. Same shape/pattern as
 * handle_set_mailbox above (NVS_NS_FLOOD instead of NVS_NS_MAILBOX). */
static int handle_set_flood_transport(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    cJSON* enabled = cJSON_GetObjectItem(params, "enabled");
    if (!enabled || !cJSON_IsBool(enabled))
        return RPC_ERR_INVALID_PARAMS;

    /* Persist to NVS */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_FLOOD, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "enabled", cJSON_IsTrue(enabled) ? 1 : 0);
        err = rpc_nvs_commit_close(nvs, err);
    }

    if (err != ESP_OK) {
        return rpc_report_persist_failure(result, "flood transport persist failed", err, true,
                                          "nvs write failed", 0);
    }

    bool en = cJSON_IsTrue(enabled);
    mesh_set_flood_transport(en);

    ESP_LOGI("rpc", "Flood transport %s", en ? "enabled" : "disabled");
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddBoolToObject(result, "enabled", en);
    return 0;
}

/* Flooding F1 finalize: bramble.setFloodHopLimit {hops}. Sets the operator-
 * settable flood-transport origination hop budget. mesh_set_flood_hop_limit
 * clamps to [FLOOD_HOP_LIMIT_MIN, FLOOD_HOP_LIMIT_CEIL]; the CLAMPED value is
 * what we persist and echo back, so a client always sees exactly what took
 * effect. Only the flood transport uses this; ROUTE_HOP_LIMIT_MAX (reactive)
 * is untouched. */
static int handle_set_flood_hop_limit(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    cJSON* hops = cJSON_GetObjectItem(params, "hops");
    if (!hops || !cJSON_IsNumber(hops))
        return RPC_ERR_INVALID_PARAMS;

    /* Clamp inside mesh_set_flood_hop_limit, then read back the applied value. */
    mesh_set_flood_hop_limit((uint32_t)hops->valuedouble);
    uint8_t applied = mesh_get_flood_hop_limit();

    /* Persist the clamped value to NVS. */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_FLOOD, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "hop_limit", applied);
        err = rpc_nvs_commit_close(nvs, err);
    }

    if (err != ESP_OK) {
        /* Hop limit is already applied live above; only persistence
         * failed, so the change is real until the next reboot. */
        return rpc_report_persist_failure(result, "flood hop limit persist failed", err, true,
                                          "failed to persist; setting lost on reboot", 0);
    }

    ESP_LOGI("rpc", "Flood hop limit set to %u", (unsigned)applied);
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "hops", applied);
    return 0;
}

static esp_err_t location_policy_load_or_init(nvs_handle_t nvs, location_policy_t* policy) {
    if (!policy)
        return ESP_ERR_INVALID_ARG;

    location_policy_set_defaults(policy);

    esp_err_t err;
    bool write_back = false;

    uint8_t enabled = 0;
    err = nvs_get_u8(nvs, "enabled", &enabled);
    if (err == ESP_OK) {
        policy->enabled = (enabled != 0);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        write_back = true;
    } else {
        return err;
    }

    uint16_t interval_s = 0;
    err = nvs_get_u16(nvs, "interval_s", &interval_s);
    if (err == ESP_OK) {
        policy->interval_s = interval_s;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        write_back = true;
    } else {
        return err;
    }

    char tier[16] = {0};
    size_t tier_len = sizeof(tier);
    err = nvs_get_str(nvs, "def_tier", tier, &tier_len);
    if (err == ESP_OK) {
        policy->default_tier = location_tier_from_string(tier);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        write_back = true;
    } else {
        return err;
    }

    uint16_t normalized_interval = policy->interval_s;
    location_policy_normalize(policy);
    if (normalized_interval != policy->interval_s) {
        write_back = true;
    }

    if (write_back) {
        err = nvs_set_u8(nvs, "enabled", policy->enabled ? 1 : 0);
        if (err == ESP_OK)
            err = nvs_set_u16(nvs, "interval_s", policy->interval_s);
        if (err == ESP_OK)
            err = nvs_set_str(nvs, "def_tier", location_tier_to_string(policy->default_tier));
        if (err == ESP_OK)
            err = nvs_commit(nvs);
        return err;
    }

    return ESP_OK;
}

static const char* rpc_location_source_normalize(const char* source) {
    if (!source || source[0] == '\0')
        return "hybrid";
    if (strcmp(source, "gps") == 0)
        return "gps";
    if (strcmp(source, "manual") == 0)
        return "manual";
    if (strcmp(source, "hybrid") == 0)
        return "hybrid";
    return "hybrid";
}

/* Build a per-target location rule from a request JSON entry, starting from the
   node policy's default tier and interval and letting the entry override any of
   enabled/tier/interval_s. Shared by the contact_rules and channel_targets
   write loops in set_location_config, which differ only in how the rule is
   keyed into NVS. */
static location_rule_t rpc_location_rule_from_json(const cJSON* entry,
                                                       const location_policy_t* policy) {
    location_rule_t rule = {
        .enabled = true,
        .tier = policy->default_tier,
        .interval_s = policy->interval_s,
    };

    const cJSON* rule_enabled = cJSON_GetObjectItem(entry, "enabled");
    if (rule_enabled && cJSON_IsBool(rule_enabled))
        rule.enabled = cJSON_IsTrue(rule_enabled);

    const cJSON* rule_tier = cJSON_GetObjectItem(entry, "tier");
    if (rule_tier && cJSON_IsString(rule_tier))
        rule.tier = location_tier_from_string(rule_tier->valuestring);

    const cJSON* rule_interval = cJSON_GetObjectItem(entry, "interval_s");
    if (rule_interval && cJSON_IsNumber(rule_interval)) {
        int v = rule_interval->valueint;
        if (v > 0)
            rule.interval_s = location_policy_clamp_interval_s((uint16_t)v);
    }
    return rule;
}

/* Parse a stored rule string and append its enabled/tier/interval_s fields to a
   response entry. The caller adds the identifying field (address or channel)
   first. Shared by the contact_rules and channel_targets read loops in
   get_location_config. */
static void rpc_location_rule_emit_fields(cJSON* entry, const char* raw) {
    location_rule_t rule = {
        .enabled = true,
        .tier = LOCATION_TIER_COARSE,
        .interval_s = LOCATION_DEFAULT_INTERVAL_S,
    };
    location_rule_parse(raw, &rule);
    cJSON_AddBoolToObject(entry, "enabled", rule.enabled);
    cJSON_AddStringToObject(entry, "tier", location_tier_to_string(rule.tier));
    cJSON_AddNumberToObject(entry, "interval_s", rule.interval_s);
}

static int handle_set_location_config(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;

    /* Validate every channel target before writing any of this config. A
       channel index outside the key space resolves to no target at all, and
       accepting it would leave the caller with a policy that reads back as
       configured while the send path has nothing to send to. Rejecting up
       front also keeps the write below all-or-nothing. */
    const cJSON* proposed_targets = cJSON_GetObjectItem(params, "channel_targets");
    if (proposed_targets && cJSON_IsArray(proposed_targets)) {
        const cJSON* proposed = NULL;
        cJSON_ArrayForEach(proposed, proposed_targets) {
            const cJSON* channel = cJSON_GetObjectItem(proposed, "channel");
            char probe[LOCATION_TARGET_KEY_SIZE];
            if (!cJSON_IsNumber(channel) ||
                !location_channel_key(probe, sizeof(probe), channel->valueint)) {
                return RPC_ERR_INVALID_PARAMS;
            }
        }
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs) != ESP_OK) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "NVS open failed");
        return 0;
    }

    location_policy_t policy;
    if (location_policy_load_or_init(nvs, &policy) != ESP_OK) {
        nvs_close(nvs);
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "NVS read failed");
        return 0;
    }

    cJSON* enabled = cJSON_GetObjectItem(params, "enabled");
    if (enabled && cJSON_IsBool(enabled)) {
        policy.enabled = cJSON_IsTrue(enabled);
    }

    cJSON* interval = cJSON_GetObjectItem(params, "interval_s");
    if (interval && cJSON_IsNumber(interval)) {
        int interval_val = interval->valueint;
        if (interval_val < 0)
            interval_val = 0;
        policy.interval_s = location_policy_clamp_interval_s((uint16_t)interval_val);
    }

    cJSON* default_tier = cJSON_GetObjectItem(params, "default_tier");
    if (default_tier && cJSON_IsString(default_tier)) {
        policy.default_tier = location_tier_from_string(default_tier->valuestring);
    }

    esp_err_t err = ESP_OK;
    cJSON* source = cJSON_GetObjectItem(params, "source");
    if (source && cJSON_IsString(source)) {
        err = nvs_set_str(nvs, LOCATION_SOURCE_KEY,
                          rpc_location_source_normalize(source->valuestring));
    }

    location_policy_normalize(&policy);
    if (err == ESP_OK)
        err = nvs_set_u8(nvs, "enabled", policy.enabled ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_set_u16(nvs, "interval_s", policy.interval_s);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "def_tier", location_tier_to_string(policy.default_tier));

    cJSON* contact_rules = cJSON_GetObjectItem(params, "contact_rules");
    if (contact_rules && cJSON_IsArray(contact_rules)) {
        const cJSON* entry = NULL;
        cJSON_ArrayForEach(entry, contact_rules) {
            const cJSON* address = cJSON_GetObjectItem(entry, "address");
            if (!cJSON_IsString(address) || !address->valuestring)
                continue;

            location_rule_t rule = rpc_location_rule_from_json(entry, &policy);

            char key[20];
            char val[48];
            snprintf(key, sizeof(key), LOCATION_CONTACT_RULE_PREFIX "%.8s", address->valuestring);
            location_rule_format(val, sizeof(val), &rule);
            if (err == ESP_OK)
                err = nvs_set_str(nvs, key, val);
        }
    }

    cJSON* channel_targets = cJSON_GetObjectItem(params, "channel_targets");
    if (channel_targets && cJSON_IsArray(channel_targets)) {
        const cJSON* entry = NULL;
        cJSON_ArrayForEach(entry, channel_targets) {
            const cJSON* channel = cJSON_GetObjectItem(entry, "channel");
            char key[LOCATION_TARGET_KEY_SIZE];
            /* Range-checked above, before anything was written. */
            if (!location_channel_key(key, sizeof(key), channel->valueint))
                continue;

            location_rule_t rule = rpc_location_rule_from_json(entry, &policy);

            char val[48];
            location_rule_format(val, sizeof(val), &rule);
            if (err == ESP_OK)
                err = nvs_set_str(nvs, key, val);
        }
    }

    /* Accept manual coordinates (no GPS hardware on Heltec V3) */
    cJSON* lat = cJSON_GetObjectItem(params, "lat");
    cJSON* lon = cJSON_GetObjectItem(params, "lon");
    if (lat && cJSON_IsNumber(lat) && err == ESP_OK)
        err = nvs_set_i32(nvs, "lat_e6", (int32_t)(lat->valuedouble * 1e6));
    if (lon && cJSON_IsNumber(lon) && err == ESP_OK)
        err = nvs_set_i32(nvs, "lon_e6", (int32_t)(lon->valuedouble * 1e6));

    err = rpc_nvs_commit_close(nvs, err);

    if (err != ESP_OK) {
        return rpc_report_persist_failure(result, "location config persist failed", err, true,
                                          "nvs write failed", 0);
    }

    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

static int handle_set_location_contact(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    const char* addr_str = cJSON_GetStringValue(cJSON_GetObjectItem(params, "address"));
    const char* tier = cJSON_GetStringValue(cJSON_GetObjectItem(params, "tier"));
    if (!addr_str)
        return RPC_ERR_INVALID_PARAMS;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %d", err);
        cJSON_AddBoolToObject(result, "ok", false);
        return 0;
    }

    location_rule_t rule = {
        .enabled = true,
        .tier = tier ? location_tier_from_string(tier) : LOCATION_TIER_COARSE,
        .interval_s = LOCATION_DEFAULT_INTERVAL_S,
    };
    cJSON* enabled = cJSON_GetObjectItem(params, "enabled");
    cJSON* interval_s = cJSON_GetObjectItem(params, "interval_s");
    if (enabled && cJSON_IsBool(enabled))
        rule.enabled = cJSON_IsTrue(enabled);
    if (interval_s && cJSON_IsNumber(interval_s) && interval_s->valueint > 0) {
        rule.interval_s = location_policy_clamp_interval_s((uint16_t)interval_s->valueint);
    }

    char key[20];
    char rule_buf[48];
    location_rule_format(rule_buf, sizeof(rule_buf), &rule);
    snprintf(key, sizeof(key), LOCATION_CONTACT_RULE_PREFIX "%.8s", addr_str);
    err = nvs_set_str(nvs, key, rule_buf);
    err = rpc_nvs_commit_close(nvs, err);

    if (err != ESP_OK) {
        return rpc_report_persist_failure(result, "location contact persist failed", err, true,
                                          "nvs write failed", 0);
    }

    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

static int handle_remove_location_contact(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    const char* addr_str = cJSON_GetStringValue(cJSON_GetObjectItem(params, "address"));
    if (!addr_str)
        return RPC_ERR_INVALID_PARAMS;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %d", err);
        cJSON_AddBoolToObject(result, "ok", false);
        return 0;
    }

    char key[20];
    snprintf(key, sizeof(key), LOCATION_CONTACT_RULE_PREFIX "%.8s", addr_str);
    err = nvs_erase_key(nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK; /* nothing to remove is not a failure */
    err = rpc_nvs_commit_close(nvs, err);

    if (err != ESP_OK) {
        return rpc_report_persist_failure(result, "location contact remove failed", err, true,
                                          "nvs write failed", 0);
    }

    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

static int handle_share_location_once(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    const char* addr_str = cJSON_GetStringValue(cJSON_GetObjectItem(params, "address"));
    if (!addr_str)
        return RPC_ERR_INVALID_PARAMS;

    /* Read stored location from NVS */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs) != ESP_OK) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "no location configured");
        return 0;
    }
    location_policy_t policy;
    if (location_policy_load_or_init(nvs, &policy) != ESP_OK) {
        nvs_close(nvs);
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "location policy read failed");
        return 0;
    }
    nvs_close(nvs);

    bramble_position_t pos;
    if (!mesh_resolve_self_position(&pos)) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error",
                                "no location available (no GPS fix and no manual location set)");
        return 0;
    }
    pos.timestamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    uint8_t tier = policy.default_tier;
    cJSON* tier_j = cJSON_GetObjectItem(params, "tier");
    if (tier_j && cJSON_IsString(tier_j)) {
        tier = location_tier_from_string(tier_j->valuestring);
    }

    uint32_t dest_addr = (uint32_t)strtoul(addr_str, NULL, 16);
    uint32_t pkt_id = mesh_send_location_packet(dest_addr, &pos, tier);
    if (pkt_id == 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "send failed (no route or radio busy)");
        return 0;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "lat", pos.latitude_e7 / 1e7);
    cJSON_AddNumberToObject(result, "lon", pos.longitude_e7 / 1e7);
    cJSON_AddStringToObject(result, "tier", location_tier_to_string(tier));
    char pkt_buf[12];
    snprintf(pkt_buf, sizeof(pkt_buf), "%08" PRIX32, pkt_id);
    cJSON_AddStringToObject(result, "packetId", pkt_buf);
    return 0;
}

/* bramble.getMessages: returns stored messages from ring buffer */
static int handle_get_messages(const cJSON* params, cJSON* result) {
    (void)params;
    cJSON* arr = cJSON_AddArrayToObject(result, "messages");
    char buf[12];

    int count = msg_store_count();
    for (int i = 0; i < count; i++) {
        const stored_msg_t* m = msg_store_get(i);
        if (!m)
            continue;

        cJSON* obj = cJSON_CreateObject();
        char buf2[12];
        bool is_out = (m->direction == MSG_DIR_OUTGOING || m->direction == MSG_DIR_BROADCAST_OUT);
        cJSON_AddStringToObject(obj, "from",
                                is_out ? addr_hex(s_identity->address, buf, sizeof(buf))
                                       : addr_hex(m->peer_addr, buf, sizeof(buf)));
        cJSON_AddStringToObject(obj, "to",
                                is_out ? addr_hex(m->peer_addr, buf2, sizeof(buf2))
                                       : addr_hex(s_identity->address, buf2, sizeof(buf2)));

        const char* dir_str = "incoming";
        switch (m->direction) {
        case MSG_DIR_OUTGOING:
            dir_str = "outgoing";
            break;
        case MSG_DIR_BROADCAST_IN:
            dir_str = "broadcast_in";
            break;
        case MSG_DIR_BROADCAST_OUT:
            dir_str = "broadcast_out";
            break;
        default:
            break;
        }
        cJSON_AddStringToObject(obj, "direction", dir_str);
        cJSON_AddStringToObject(obj, "text", m->text);
        cJSON_AddNumberToObject(obj, "channel", m->channel_index);
        cJSON_AddBoolToObject(
            obj, "broadcast",
            (m->direction == MSG_DIR_BROADCAST_IN || m->direction == MSG_DIR_BROADCAST_OUT));
        cJSON_AddNumberToObject(obj, "timestamp_s", m->timestamp_s);
        if (m->rssi != 0)
            cJSON_AddNumberToObject(obj, "rssi", m->rssi);
        if (m->snr != 0)
            cJSON_AddNumberToObject(obj, "snr", m->snr);
        static const char* status_names[] = {"none", "sent", "delivered", "failed"};
        if (m->status > 0 && m->status <= 3) {
            cJSON_AddStringToObject(obj, "status", status_names[m->status]);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    return 0;
}

/* bramble.getPeerLocations: returns own location + any received peer locations */
static int handle_get_peer_locations(const cJSON* params, cJSON* result) {
    (void)params;
    cJSON* peer_locations = cJSON_AddArrayToObject(result, "peerLocations");

    /* Open NVS once for both own location and peer entries.
     * READWRITE needed because location_policy_load_or_init may write defaults. */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs) == ESP_OK) {
        /* Include own location if set */
        location_policy_t policy;
        if (location_policy_load_or_init(nvs, &policy) == ESP_OK) {
            int32_t lat_e6 = 0, lon_e6 = 0;
            nvs_get_i32(nvs, "lat_e6", &lat_e6);
            nvs_get_i32(nvs, "lon_e6", &lon_e6);

            /* Prefer the live GPS fix over manually configured coordinates,
             * mirroring mesh_location_policy_tick: a GPS-only node has
             * lat_e6 == lon_e6 == 0 in NVS and was previously omitted
             * entirely, leaving the map empty. */
            bramble_position_t gps_pos;
            bool has_gps = gps_get_position(&gps_pos) && gps_pos.valid;
            /* Not gated on policy.enabled: the sharing policy governs what is
             * broadcast to the mesh, not whether the owner sees their own
             * position on their own map. */
            (void)policy;
            if (has_gps || lat_e6 != 0 || lon_e6 != 0) {
                cJSON* self = cJSON_CreateObject();
                char buf[12];
                cJSON* position = cJSON_CreateObject();
                if (has_gps) {
                    cJSON_AddNumberToObject(position, "lat", gps_pos.latitude_e7 / 1e7);
                    cJSON_AddNumberToObject(position, "lon", gps_pos.longitude_e7 / 1e7);
                    cJSON_AddNumberToObject(position, "alt", gps_pos.altitude_m);
                    cJSON_AddNumberToObject(position, "accuracy", gps_pos.accuracy_m);
                    cJSON_AddNumberToObject(position, "speed", gps_pos.speed_kmh);
                    cJSON_AddNumberToObject(position, "heading", gps_pos.heading_deg2 * 2);
                } else {
                    cJSON_AddNumberToObject(position, "lat", lat_e6 / 1e6);
                    cJSON_AddNumberToObject(position, "lon", lon_e6 / 1e6);
                    cJSON_AddNumberToObject(position, "alt", 0);
                    cJSON_AddNumberToObject(position, "accuracy", 0);
                    cJSON_AddNumberToObject(position, "speed", 0);
                    cJSON_AddNumberToObject(position, "heading", 0);
                }
                cJSON_AddNumberToObject(position, "timestampMs",
                                        (double)(esp_timer_get_time() / 1000ULL));

                cJSON_AddStringToObject(self, "addr",
                                        addr_hex(s_identity->address, buf, sizeof(buf)));
                cJSON_AddStringToObject(self, "name", "self");
                cJSON_AddStringToObject(self, "tier", "full");
                cJSON_AddItemToObject(self, "position", position);
                cJSON_AddBoolToObject(self, "online", true);
                cJSON_AddNumberToObject(self, "lastUpdatedMs",
                                        (double)(esp_timer_get_time() / 1000ULL));
                cJSON_AddItemToArray(peer_locations, self);
            }
        }

        /* Include received peer locations persisted by mesh location RX path. */
        {
            nvs_iterator_t it = NULL;
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

            if (nvs_entry_find(NVS_PARTITION, NVS_NS_LOCATION, NVS_TYPE_ANY, &it) == ESP_OK) {
                while (it != NULL) {
                    nvs_entry_info_t info;
                    nvs_entry_info(it, &info);

                    if (strncmp(info.key, "lp_", 3) == 0) {
                        persisted_peer_location_t stored = {0};
                        size_t len = sizeof(stored);
                        if (nvs_get_blob(nvs, info.key, &stored, &len) == ESP_OK &&
                            len == sizeof(stored)) {
                            cJSON* peer = cJSON_CreateObject();
                            uint32_t freshness_ms =
                                (now_ms >= stored.received_ms) ? (now_ms - stored.received_ms) : 0;

                            cJSON* position = cJSON_CreateObject();
                            cJSON_AddNumberToObject(position, "lat", stored.latitude_e7 / 1e7);
                            cJSON_AddNumberToObject(position, "lon", stored.longitude_e7 / 1e7);
                            cJSON_AddNumberToObject(position, "alt", stored.altitude_m);
                            cJSON_AddNumberToObject(position, "accuracy", stored.accuracy_m);
                            cJSON_AddNumberToObject(position, "speed", stored.speed_kmh);
                            cJSON_AddNumberToObject(position, "heading", stored.heading_deg2 * 2);
                            cJSON_AddNumberToObject(position, "timestampMs",
                                                    (double)stored.timestamp * 1000.0);

                            cJSON_AddStringToObject(peer, "addr", info.key + 3);
                            cJSON_AddStringToObject(peer, "name", "");
                            cJSON_AddStringToObject(peer, "tier",
                                                    location_tier_to_string(stored.tier));
                            cJSON_AddItemToObject(peer, "position", position);
                            cJSON_AddBoolToObject(peer, "online",
                                                  freshness_ms < LOCATION_CACHE_TTL_MS);
                            cJSON_AddNumberToObject(peer, "lastUpdatedMs", stored.received_ms);

                            cJSON_AddItemToArray(peer_locations, peer);
                        }
                    }

                    if (nvs_entry_next(&it) != ESP_OK) {
                        break;
                    }
                }
                nvs_release_iterator(it);
            }
        } /* end peer locations block */
        nvs_close(nvs);
    }

    return 0;
}

static bool rpc_get_persisted_channel_name(int index, char* name_out, size_t name_out_len) {
    if (!name_out || name_out_len == 0 || index < 0)
        return false;

    nvs_handle_t ch_nvs;
    if (nvs_open(NVS_NS_CHANNEL, NVS_READONLY, &ch_nvs) != ESP_OK) {
        return false;
    }

    char key_name[20];
    size_t len = name_out_len;
    snprintf(key_name, sizeof(key_name), "nm%d", index);
    esp_err_t err = nvs_get_str(ch_nvs, key_name, name_out, &len);

    if (err != ESP_OK || name_out[0] == '\0') {
        len = name_out_len;
        snprintf(key_name, sizeof(key_name), "ch%d_name", index);
        err = nvs_get_str(ch_nvs, key_name, name_out, &len);
    }

    nvs_close(ch_nvs);
    return (err == ESP_OK && name_out[0] != '\0');
}

static bool rpc_get_persisted_channel_has_psk(int index, bool* has_psk_out) {
    if (!has_psk_out || index < 0)
        return false;

    nvs_handle_t ch_nvs;
    if (nvs_open(NVS_NS_CHANNEL, NVS_READONLY, &ch_nvs) != ESP_OK) {
        return false;
    }

    char key[16];
    uint8_t has_psk = 0;
    snprintf(key, sizeof(key), "psk%d", index);
    esp_err_t err = nvs_get_u8(ch_nvs, key, &has_psk);
    nvs_close(ch_nvs);
    if (err != ESP_OK)
        return false;

    *has_psk_out = (has_psk != 0);
    return true;
}

/* bramble.getConfig: returns node name + radio config + channel list */
static int handle_get_config(const cJSON* params, cJSON* result) {
    (void)params;

    /* Node name from NVS (falls back to "(unnamed)" if not set) */
    char name[64] = "(unnamed)";
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(name);
        nvs_get_str(nvs, NVS_KEY_NODE_NAME, name, &len);
        nvs_close(nvs);
    }
    cJSON_AddStringToObject(result, "node_name", name);

    /* Node address */
    char buf[12];
    cJSON_AddStringToObject(result, "address", addr_hex(s_identity->address, buf, sizeof(buf)));
    cJSON_AddStringToObject(result, "pubkey_hash",
                            addr_hex(s_identity->pubkey_hash, buf, sizeof(buf)));

    /* Radio config: read actual runtime state */
    radio_config_t rcfg;
    radio_get_config(&rcfg);
    cJSON* radio = cJSON_CreateObject();
    cJSON_AddNumberToObject(radio, "frequency_mhz", rcfg.frequency_mhz);
    cJSON_AddNumberToObject(radio, "sf", rcfg.sf);
    cJSON_AddNumberToObject(radio, "bw_hz", rcfg.bw_hz);
    cJSON_AddNumberToObject(radio, "tx_power_dbm", rcfg.tx_power);
    cJSON_AddNumberToObject(radio, "coding_rate", rcfg.coding_rate);
    cJSON_AddStringToObject(radio, "profile", "custom");
    cJSON_AddItemToObject(result, "radio", radio);

    /* Channel list: read from mesh runtime state */
    int default_channel = 0;
    int ch_count = mesh_get_channel_info(&default_channel);
    cJSON* channels = cJSON_CreateArray();

    for (int i = 0; i < ch_count; i++) {
        cJSON* ch = cJSON_CreateObject();
        const char* ch_name = mesh_get_channel_name(i);
        bool has_psk = false;
        uint16_t epoch = 0;
        mesh_get_channel_security(i, &has_psk, &epoch);

        char persisted_name[20] = {0};
        if (!ch_name || ch_name[0] == '\0') {
            if (rpc_get_persisted_channel_name(i, persisted_name, sizeof(persisted_name))) {
                ch_name = persisted_name;
            }
        }

        if (!has_psk) {
            bool persisted_has_psk = false;
            if (rpc_get_persisted_channel_has_psk(i, &persisted_has_psk)) {
                has_psk = persisted_has_psk;
            }
        }

        char fallback[20];
        if (!ch_name || ch_name[0] == '\0') {
            snprintf(fallback, sizeof(fallback), "channel_%d", i);
            ch_name = fallback;
        }

        cJSON_AddStringToObject(ch, "name", ch_name);
        cJSON_AddNumberToObject(ch, "id", i);
        cJSON_AddBoolToObject(ch, "is_default", i == default_channel);
        cJSON_AddBoolToObject(ch, "hasPsk", has_psk);
        cJSON_AddNumberToObject(ch, "epoch", epoch);
        cJSON_AddItemToArray(channels, ch);
    }

    cJSON_AddItemToObject(result, "channels", channels);

    /* Location sharing policy contract (hybrid privacy-first). */
    cJSON* location = cJSON_CreateObject();
    if (nvs_open(NVS_NS_LOCATION, NVS_READONLY, &nvs) == ESP_OK) {
        location_policy_t policy;
        if (location_policy_load_or_init(nvs, &policy) == ESP_OK) {
            cJSON_AddBoolToObject(location, "enabled", policy.enabled);
            cJSON_AddStringToObject(location, "tier", location_tier_to_string(policy.default_tier));
            cJSON_AddStringToObject(location, "default_tier",
                                    location_tier_to_string(policy.default_tier));
            cJSON_AddNumberToObject(location, "interval_s", policy.interval_s);
        } else {
            cJSON_AddBoolToObject(location, "enabled", false);
            cJSON_AddStringToObject(location, "tier", "coarse");
            cJSON_AddStringToObject(location, "default_tier", "coarse");
            cJSON_AddNumberToObject(location, "interval_s", LOCATION_DEFAULT_INTERVAL_S);
        }

        char source_buf[16] = {0};
        size_t source_len = sizeof(source_buf);
        if (nvs_get_str(nvs, LOCATION_SOURCE_KEY, source_buf, &source_len) == ESP_OK) {
            cJSON_AddStringToObject(location, "source", rpc_location_source_normalize(source_buf));
        } else {
            cJSON_AddStringToObject(location, "source", "hybrid");
        }

        int32_t lat_e6 = 0, lon_e6 = 0;
        if (nvs_get_i32(nvs, "lat_e6", &lat_e6) == ESP_OK)
            cJSON_AddNumberToObject(location, "lat", lat_e6 / 1e6);
        if (nvs_get_i32(nvs, "lon_e6", &lon_e6) == ESP_OK)
            cJSON_AddNumberToObject(location, "lon", lon_e6 / 1e6);

        cJSON* contact_rules = cJSON_AddArrayToObject(location, "contact_rules");
        cJSON* channel_targets = cJSON_AddArrayToObject(location, "channel_targets");

        nvs_iterator_t it = NULL;
        if (nvs_entry_find(NVS_PARTITION, NVS_NS_LOCATION, NVS_TYPE_ANY, &it) == ESP_OK) {
            while (it != NULL) {
                nvs_entry_info_t info;
                nvs_entry_info(it, &info);

                if (strncmp(info.key, LOCATION_CONTACT_RULE_PREFIX,
                            strlen(LOCATION_CONTACT_RULE_PREFIX)) == 0) {
                    const char* addr_suffix = info.key + strlen(LOCATION_CONTACT_RULE_PREFIX);

                    char raw[64] = {0};
                    size_t raw_len = sizeof(raw);
                    if (nvs_get_str(nvs, info.key, raw, &raw_len) == ESP_OK) {
                        cJSON* entry = cJSON_CreateObject();
                        cJSON_AddStringToObject(entry, "address", addr_suffix);
                        rpc_location_rule_emit_fields(entry, raw);
                        cJSON_AddItemToArray(contact_rules, entry);
                    }
                }

                if (strncmp(info.key, LOCATION_CHANNEL_RULE_PREFIX,
                            strlen(LOCATION_CHANNEL_RULE_PREFIX)) == 0) {
                    char raw[64] = {0};
                    size_t raw_len = sizeof(raw);
                    if (nvs_get_str(nvs, info.key, raw, &raw_len) == ESP_OK) {
                        cJSON* entry = cJSON_CreateObject();
                        cJSON_AddNumberToObject(
                            entry, "channel",
                            atoi(info.key + strlen(LOCATION_CHANNEL_RULE_PREFIX)));
                        rpc_location_rule_emit_fields(entry, raw);
                        cJSON_AddItemToArray(channel_targets, entry);
                    }
                }

                if (nvs_entry_next(&it) != ESP_OK) {
                    break;
                }
            }
            nvs_release_iterator(it);
        }

        nvs_close(nvs);
    } else {
        cJSON_AddBoolToObject(location, "enabled", false);
        cJSON_AddStringToObject(location, "tier", "coarse");
        cJSON_AddStringToObject(location, "default_tier", "coarse");
        cJSON_AddNumberToObject(location, "interval_s", LOCATION_DEFAULT_INTERVAL_S);
        cJSON_AddStringToObject(location, "source", "hybrid");
        cJSON_AddItemToObject(location, "contact_rules", cJSON_CreateArray());
        cJSON_AddItemToObject(location, "channel_targets", cJSON_CreateArray());
    }
    cJSON_AddItemToObject(result, "location", location);

    const char* mode = "recipient_only";
    switch (mesh_get_broadcast_telemetry_mode()) {
    case BROADCAST_TELEMETRY_OFF:
        mode = "off";
        break;
    case BROADCAST_TELEMETRY_PATH_SAMPLED:
        mode = "path_sampled";
        break;
    case BROADCAST_TELEMETRY_RECIPIENT_ONLY:
    default:
        mode = "recipient_only";
        break;
    }
    cJSON_AddStringToObject(result, "broadcast_telemetry_mode", mode);

    /* Mailbox enabled state */
    cJSON_AddBoolToObject(result, "mailboxEnabled", mesh_get_mailbox());

    /* Flooding F1 Task 1: flood transport toggle state */
    cJSON_AddBoolToObject(result, "floodTransportEnabled", mesh_get_flood_transport());

    /* Flooding F1 finalize: operator-settable flood origination hop budget. */
    cJSON_AddNumberToObject(result, "floodHopLimit", mesh_get_flood_hop_limit());

    return 0;
}

static int handle_set_broadcast_telemetry_mode(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    const char* mode = cJSON_GetStringValue(cJSON_GetObjectItem(params, "mode"));
    if (!mode)
        return RPC_ERR_INVALID_PARAMS;

    broadcast_telemetry_mode_t m;
    if (strcmp(mode, "off") == 0) {
        m = BROADCAST_TELEMETRY_OFF;
    } else if (strcmp(mode, "recipient_only") == 0) {
        m = BROADCAST_TELEMETRY_RECIPIENT_ONLY;
    } else if (strcmp(mode, "path_sampled") == 0) {
        m = BROADCAST_TELEMETRY_PATH_SAMPLED;
    } else {
        return RPC_ERR_INVALID_PARAMS;
    }

    mesh_set_broadcast_telemetry_mode(m);
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "broadcast_telemetry_mode", mode);
    return 0;
}

/* ── Registration ───────────────────────────────────────────────────── */

/* OTA task: runs in background after RPC response.
 * Stack sizing: esp_ota_end runs the full image verification on this task's
 * stack, including mbedtls RSA-3072 PSS signature checks
 * (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT). 8192 bytes overflows
 * there and corrupts adjacent heap (same failure class as the 16KB ble_rpc
 * stack fix); the overflow surfaced as StoreProhibited in esp_ota_end's
 * LIST_REMOVE after a successful signature verify. */
#define OTA_TASK_STACK_SIZE 16384

static volatile bool s_ota_in_progress = false;

typedef struct {
    char* url;
    bool allow_downgrade;
} ota_task_args_t;

static void ota_task(void* arg) {
    ota_task_args_t* args = (ota_task_args_t*)arg;
    vTaskDelay(pdMS_TO_TICKS(500)); /* let RPC response flush */
    int rc = ota_wifi_start(args->url, args->allow_downgrade);
    free(args->url);
    free(args);
    s_ota_in_progress = false;
    ESP_LOGI("ota", "ota_task stack high-water mark: %u bytes free of %u",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)OTA_TASK_STACK_SIZE);
    if (rc == 0) {
        ota_progress_set_state(OTA_PROG_REBOOTING);
        ESP_LOGI("ota", "OTA complete; rebooting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    ESP_LOGE("ota", "OTA failed: %s",
             ota_get_last_error() ? ota_get_last_error() : "unknown error");
    vTaskDelete(NULL);
}

/* bramble.otaUpdate: start OTA from a relative artifact path resolved
 * against the allowlisted origin. Raw URLs are not accepted. */
static int handle_ota_update(const cJSON* params, cJSON* result) {
    if (cJSON_GetObjectItem(params, "url")) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error",
                                "raw URLs are not accepted; pass 'path' relative to the "
                                "configured OTA origin (see bramble.otaGetOrigin)");
        return 0;
    }

    const char* path = cJSON_GetStringValue(cJSON_GetObjectItem(params, "path"));
    if (!path || strlen(path) == 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error",
                                "path parameter required (artifact path relative to the OTA "
                                "origin, e.g. stable/v1.4.0/heltec-v3/bramble.bin)");
        return 0;
    }

    bool allow_downgrade = cJSON_IsTrue(cJSON_GetObjectItem(params, "allow_downgrade"));

    char url[OTA_URL_MAX];
    int rc = ota_resolve_artifact(path, url, sizeof(url));
    if (rc != OTA_URL_OK) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error",
                                rc == OTA_URL_ERR_PATH
                                    ? "invalid artifact path (relative paths only: no absolute "
                                      "URLs, no traversal, no special characters)"
                                    : "OTA origin rejected by policy");
        return 0;
    }

    if (s_ota_in_progress) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "OTA already in progress");
        return 0;
    }

    const char* last_error = ota_get_last_error();
    if (last_error) {
        cJSON_AddStringToObject(result, "last_error", last_error);
    }

    /* Copy args to heap so the task owns them */
    ota_task_args_t* args = calloc(1, sizeof(*args));
    char* url_copy = strdup(url);
    if (!args || !url_copy) {
        free(args);
        free(url_copy);
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "out of memory");
        return 0;
    }
    args->url = url_copy;
    args->allow_downgrade = allow_downgrade;

    s_ota_in_progress = true;

    /* A stale terminal snapshot from the previous attempt must not be
     * observable once a new update is accepted; otherwise a poller that
     * reads bramble.otaStatus before the task's first progress report
     * sees the old attempt's outcome (e.g. "failed") instead of this one. */
    ota_progress_report(OTA_PROG_IDLE, 0, 0);

    if (xTaskCreate(ota_task, "ota", OTA_TASK_STACK_SIZE, args, 3, NULL) != pdPASS) {
        s_ota_in_progress = false;
        free(url_copy);
        free(args);
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "failed to start OTA task");
        return 0;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "note", "OTA started; waiting for reboot confirms success");
    cJSON_AddStringToObject(result, "url", url);
    cJSON_AddStringToObject(result, "partition", ota_get_running_partition());
    return 0;
}

/* bramble.otaGetOrigin: report the effective OTA origin and rollback floor */
static int handle_ota_get_origin(const cJSON* params, cJSON* result) {
    (void)params;
    char origin[OTA_URL_MAX];
    ota_origin_get(origin, sizeof(origin));
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "origin", origin);
    cJSON_AddStringToObject(result, "default_origin", OTA_DEFAULT_ORIGIN);
    cJSON_AddBoolToObject(result, "overridden", ota_origin_is_overridden());
    char floor_str[48];
    if (ota_rollback_get_floor(floor_str, sizeof(floor_str))) {
        cJSON_AddStringToObject(result, "version_floor", floor_str);
    }
    cJSON_AddStringToObject(result, "running_version", ota_get_app_version());
    return 0;
}

/* bramble.otaStatus: live progress of the background OTA task */
static int handle_ota_status(const cJSON* params, cJSON* result) {
    (void)params;
    ota_progress_snapshot_t snap;
    ota_progress_get(&snap);

    cJSON_AddStringToObject(result, "state", ota_progress_state_str(snap.state));
    cJSON_AddNumberToObject(result, "bytes", snap.bytes);
    cJSON_AddNumberToObject(result, "total", snap.total);
    int percent = ota_progress_percent(&snap);
    cJSON_AddNumberToObject(result, "percent", percent);

    const char* last_error = ota_get_last_error();
    if (last_error && last_error[0]) {
        cJSON_AddStringToObject(result, "last_error", last_error);
    }
    cJSON_AddStringToObject(result, "running_version", ota_get_app_version());

    char floor_str[48] = {0};
    if (ota_rollback_get_floor(floor_str, sizeof(floor_str))) {
        cJSON_AddStringToObject(result, "version_floor", floor_str);
    }
    return 0;
}

/* Pushed as bramble.onOtaEvent on every OTA progress callback (state
 * transitions plus every >= 5 percentage points of download progress). */
static void ota_progress_notify_cb(const ota_progress_snapshot_t* snap) {
    cJSON* params = cJSON_CreateObject();
    if (!params) {
        return;
    }
    cJSON_AddStringToObject(params, "state", ota_progress_state_str(snap->state));
    cJSON_AddNumberToObject(params, "bytes", snap->bytes);
    cJSON_AddNumberToObject(params, "total", snap->total);
    int percent = ota_progress_percent(snap);
    cJSON_AddNumberToObject(params, "percent", percent);
    if (snap->state == OTA_PROG_FAILED) {
        const char* err = ota_get_last_error();
        if (err && err[0]) {
            cJSON_AddStringToObject(params, "error", err);
        }
    }
    rpc_notify("bramble.onOtaEvent", params);
    cJSON_Delete(params);
}

/* bramble.otaSetOrigin: override (or reset) the allowlisted OTA origin */
static int handle_ota_set_origin(const cJSON* params, cJSON* result) {
    if (cJSON_IsTrue(cJSON_GetObjectItem(params, "reset"))) {
        if (ota_origin_reset() != 0) {
            cJSON_AddBoolToObject(result, "ok", false);
            cJSON_AddStringToObject(result, "error", "failed to persist OTA origin");
            return 0;
        }
    } else {
        const char* origin = cJSON_GetStringValue(cJSON_GetObjectItem(params, "origin"));
        if (!origin || strlen(origin) == 0) {
            cJSON_AddBoolToObject(result, "ok", false);
            cJSON_AddStringToObject(result, "error", "origin parameter required (or reset:true)");
            return 0;
        }
        int rc = ota_origin_set(origin);
        if (rc == -1) {
            cJSON_AddBoolToObject(result, "ok", false);
            cJSON_AddStringToObject(result, "error",
                                    "origin rejected by policy (https:// host required; no "
                                    "userinfo, query or fragment)");
            return 0;
        }
        if (rc != 0) {
            cJSON_AddBoolToObject(result, "ok", false);
            cJSON_AddStringToObject(result, "error", "failed to persist OTA origin");
            return 0;
        }
    }

    char origin_now[OTA_URL_MAX];
    ota_origin_get(origin_now, sizeof(origin_now));
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "origin", origin_now);
    cJSON_AddBoolToObject(result, "overridden", ota_origin_is_overridden());
    return 0;
}

/* bramble.sleep: enter deep sleep with optional wake timer */
static int handle_sleep(const cJSON* params, cJSON* result) {
    int wake_sec = 0;
    if (params) {
        cJSON* ws = cJSON_GetObjectItem(params, "wake_after_s");
        if (ws && cJSON_IsNumber(ws))
            wake_sec = ws->valueint;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    if (wake_sec > 0) {
        cJSON_AddNumberToObject(result, "wake_after_s", wake_sec);
        cJSON_AddStringToObject(result, "note", "Entering deep sleep with timer wake");
    } else {
        cJSON_AddStringToObject(result, "note", "Entering deep sleep (LoRa DIO1 wake only)");
    }

    /* Delay to let RPC response flush, then sleep */
    vTaskDelay(pdMS_TO_TICKS(500));

#if defined(CONFIG_IDF_TARGET_LINUX) || defined(BRAMBLE_PLATFORM_NRF)
    /* No deep sleep on the POSIX/Linux simulator. */
    ESP_LOGW(TAG, "bramble.sleep: deep sleep not supported on the simulator");
#else
    if (wake_sec > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)wake_sec * 1000000ULL);
    }

    /* Wake on LoRa DIO1 (board-configured pin): any incoming packet */
    esp_sleep_enable_ext0_wakeup(board_get_config()->radio.dio1, 1); /* wake on HIGH */

    esp_deep_sleep_start();
    /* never reached */
#endif
    return 0;
}

/* bramble.getBattery: returns battery voltage and percentage */
static int handle_get_battery(const cJSON* params, cJSON* result) {
    (void)params;
    cJSON_AddNumberToObject(result, "voltage_mv", battery_read_mv());
    cJSON_AddNumberToObject(result, "percentage", battery_read_pct());
    return 0;
}

/* bramble.setBacklight: control display backlight */
static int handle_set_backlight(const cJSON* params, cJSON* result) {
    const bramble_board_config_t* board = board_get_config();
    if (board->spi_display.backlight < 0) {
        cJSON_AddStringToObject(result, "error", "no backlight control");
        return -1;
    }

    cJSON* level = cJSON_GetObjectItem(params, "level");
    if (!level || !cJSON_IsNumber(level)) {
        return RPC_ERR_INVALID_PARAMS;
    }

    int val = level->valueint;
    uint8_t duty = (val <= 0) ? 0 : (val >= 255 ? 255 : (uint8_t)val);
    display_set_backlight(duty);
    cJSON_AddNumberToObject(result, "level", duty);
    return 0;
}

/* bramble.getGpsPosition: returns GPS position if available */
static int handle_get_gps_position(const cJSON* params, cJSON* result) {
    (void)params;
    if (!board_has_cap(BOARD_CAP_GPS)) {
        cJSON_AddStringToObject(result, "error", "gps not supported on this board");
        return RPC_ERR_NOT_SUPPORTED;
    }

    bramble_position_t pos;
    if (gps_get_position(&pos)) {
        cJSON_AddNumberToObject(result, "lat", pos.latitude_e7 / 1e7);
        cJSON_AddNumberToObject(result, "lon", pos.longitude_e7 / 1e7);
        cJSON_AddNumberToObject(result, "alt", pos.altitude_m);
        cJSON_AddNumberToObject(result, "speed_kmh", pos.speed_kmh);
        cJSON_AddNumberToObject(result, "heading_deg", pos.heading_deg2 * 2);
        cJSON_AddNumberToObject(result, "accuracy_m", pos.accuracy_m);
        cJSON_AddNumberToObject(result, "timestamp", pos.timestamp);
        cJSON_AddBoolToObject(result, "valid", true);
    } else {
        cJSON_AddBoolToObject(result, "valid", false);
    }
    return 0;
}

/* bramble.setGpsEnabled: persist the GPS power preference, then apply live. */
static int handle_set_gps_enabled(const cJSON* params, cJSON* result) {
    if (!board_has_cap(BOARD_CAP_GPS)) {
        cJSON_AddStringToObject(result, "error", "gps not supported on this board");
        return RPC_ERR_NOT_SUPPORTED;
    }
    const cJSON* en = params ? cJSON_GetObjectItem(params, "enabled") : NULL;
    if (!en || !cJSON_IsBool(en))
        return RPC_ERR_INVALID_PARAMS;
    bool enabled = cJSON_IsTrue(en);
    gps_pref_set(enabled);    /* persist first (survives a crash mid-apply) */
    gps_set_enabled(enabled); /* then apply live, same order as the UI toggle */
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddBoolToObject(result, "enabled", enabled);
    return 0;
}

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#include "sdcard.h"

/* bramble.getStorageInfo: returns SD card status */
static int handle_get_storage_info(const cJSON* params, cJSON* result) {
    (void)params;
    cJSON_AddBoolToObject(result, "sd_present", sdcard_is_present());
    if (sdcard_is_present()) {
        cJSON_AddStringToObject(result, "mount_point", sdcard_get_mount_point());
        /* Free space reporting: ESP-IDF doesn't expose statvfs, skip for now */
    }
    return 0;
}

/* bramble.playTone: play a predefined alert tone */
static int handle_play_tone(const cJSON* params, cJSON* result) {
    (void)result;
    cJSON* tone = cJSON_GetObjectItem(params, "tone");
    if (!tone || !cJSON_IsString(tone)) {
        return RPC_ERR_INVALID_PARAMS;
    }

    const char* name = tone->valuestring;
    audio_tone_t t;

    if (strcmp(name, "message_rx") == 0) {
        t = AUDIO_TONE_MESSAGE_RX;
    } else if (strcmp(name, "message_tx") == 0) {
        t = AUDIO_TONE_MESSAGE_TX;
    } else if (strcmp(name, "peer_join") == 0) {
        t = AUDIO_TONE_PEER_JOIN;
    } else if (strcmp(name, "peer_leave") == 0) {
        t = AUDIO_TONE_PEER_LEAVE;
    } else if (strcmp(name, "error") == 0) {
        t = AUDIO_TONE_ERROR;
    } else if (strcmp(name, "boot") == 0) {
        t = AUDIO_TONE_BOOT;
    } else if (strcmp(name, "gps_fix") == 0) {
        t = AUDIO_TONE_GPS_FIX;
    } else {
        ESP_LOGW(TAG, "Unknown tone: %s", name);
        return RPC_ERR_INVALID_PARAMS;
    }

    if (audio_play_tone(t) != 0) {
        ESP_LOGW(TAG, "Failed to play tone");
        return RPC_ERR_INTERNAL;
    }

    return 0;
}

/* bramble.setVolume: set audio volume 0-100 */
static int handle_set_volume(const cJSON* params, cJSON* result) {
    (void)result;
    cJSON* vol = cJSON_GetObjectItem(params, "volume");
    if (!vol || !cJSON_IsNumber(vol)) {
        return RPC_ERR_INVALID_PARAMS;
    }
    int v = (int)cJSON_GetNumberValue(vol);
    if (v < 0 || v > 100) {
        return RPC_ERR_INVALID_PARAMS;
    }
    audio_set_volume((uint8_t)v);
    return 0;
}

/* bramble.setMuted: mute or unmute audio */
static int handle_set_muted(const cJSON* params, cJSON* result) {
    (void)result;
    cJSON* muted = cJSON_GetObjectItem(params, "muted");
    if (!muted || !cJSON_IsBool(muted)) {
        return RPC_ERR_INVALID_PARAMS;
    }
    audio_set_muted(cJSON_IsTrue(muted));
    return 0;
}

/* bramble.getAudioStatus: get volume, mute, and playback state */
static int handle_get_audio_status(const cJSON* params, cJSON* result) {
    (void)params;
    cJSON_AddBoolToObject(result, "available", audio_is_available());
    cJSON_AddNumberToObject(result, "volume", audio_get_volume());
    cJSON_AddBoolToObject(result, "muted", audio_get_muted());
    cJSON_AddBoolToObject(result, "playing", audio_is_playing());
    return 0;
}
#endif /* CONFIG_BRAMBLE_BOARD_TDECK_PLUS */

/* Bench debug methods: remote screenshot + input injection.
 * Registered on every board (the RPC contract and heltec builds stay
 * uniform); the implementation is gated on CONFIG_BRAMBLE_UI_GRAPHICAL
 * (T-Deck Plus only), and boards without a graphical UI get a clean soft
 * error in the result instead of a hard RPC failure. */

#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
#include "ui_graphics.h"
#include "trackball.h"
#include "keyboard.h"
#include "ui.h"
#endif

#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
/* Standard base64 alphabet, padded. Self-contained (no mbedtls dep), same
 * approach as the display backends' fb_base64_encode
 * (components/display/include/fb_base64.h), but chunk-oriented: each call
 * encodes an independent, self-contained base64 string (its own padding),
 * so a caller can decode chunks one at a time and concatenate the raw
 * bytes without needing to track bit alignment across chunks. */
static const char s_b64_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode_chunk(const uint8_t* in, size_t len, char* out) {
    while (len >= 3) {
        uint32_t v = ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8) | in[2];
        *out++ = s_b64_tab[(v >> 18) & 0x3F];
        *out++ = s_b64_tab[(v >> 12) & 0x3F];
        *out++ = s_b64_tab[(v >> 6) & 0x3F];
        *out++ = s_b64_tab[v & 0x3F];
        in += 3;
        len -= 3;
    }
    if (len == 1) {
        uint32_t v = (uint32_t)in[0] << 16;
        *out++ = s_b64_tab[(v >> 18) & 0x3F];
        *out++ = s_b64_tab[(v >> 12) & 0x3F];
        *out++ = '=';
        *out++ = '=';
    } else if (len == 2) {
        uint32_t v = ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8);
        *out++ = s_b64_tab[(v >> 18) & 0x3F];
        *out++ = s_b64_tab[(v >> 12) & 0x3F];
        *out++ = s_b64_tab[(v >> 6) & 0x3F];
        *out++ = '=';
    }
    *out = '\0';
}
#endif /* CONFIG_BRAMBLE_UI_GRAPHICAL */

/* Serial line-buffer safety cap: each response's DECODED chunk is capped at
 * 6KB regardless of the caller's requested max_len. */
#define SCREENSHOT_CHUNK_CAP 6144

/* bramble.screenshot, params: {"capture":bool, "offset":int, "max_len":int}.
 * capture defaults true when no offset is given (first call); when true the
 * offset param is ignored and a fresh frame is captured and served from 0.
 * When false, serves the next chunk of the LAST captured frame at "offset"
 * (no re-capture), so a full frame is: capture, then repeated offset reads. */
static int handle_screenshot(const cJSON* params, cJSON* result) {
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
    (void)params;
    cJSON_AddStringToObject(result, "error", "no graphical ui");
    return 0;
#else
    bool has_offset = params && cJSON_HasObjectItem(params, "offset");
    cJSON* capture_j = params ? cJSON_GetObjectItem(params, "capture") : NULL;
    bool capture = capture_j ? cJSON_IsTrue(capture_j) : !has_offset;

    int offset = 0;
    if (has_offset && !capture) {
        cJSON* offset_j = cJSON_GetObjectItem(params, "offset");
        if (!cJSON_IsNumber(offset_j) || offset_j->valuedouble < 0)
            return RPC_ERR_INVALID_PARAMS;
        offset = (int)offset_j->valuedouble;
    }

    int max_len = SCREENSHOT_CHUNK_CAP;
    cJSON* max_len_j = params ? cJSON_GetObjectItem(params, "max_len") : NULL;
    if (cJSON_IsNumber(max_len_j) && max_len_j->valuedouble > 0) {
        max_len = (int)max_len_j->valuedouble;
        if (max_len > SCREENSHOT_CHUNK_CAP)
            max_len = SCREENSHOT_CHUNK_CAP;
    }

    if (capture) {
        if (!ui_graphics_request_screenshot(3000)) {
            return RPC_ERR_INTERNAL;
        }
        offset = 0;
    }

    size_t total = 0;
    const uint8_t* frame = ui_graphics_get_screenshot(&total);
    if (!frame) {
        return RPC_ERR_INVALID_PARAMS; /* no frame captured yet */
    }
    if ((size_t)offset > total) {
        return RPC_ERR_INVALID_PARAMS;
    }

    size_t remaining = total - (size_t)offset;
    size_t chunk_len = remaining < (size_t)max_len ? remaining : (size_t)max_len;

    char* b64 = malloc(((chunk_len + 2) / 3) * 4 + 1);
    if (!b64) {
        return RPC_ERR_INTERNAL;
    }
    b64_encode_chunk(frame + offset, chunk_len, b64);

    cJSON_AddNumberToObject(result, "width", UI_SCREENSHOT_WIDTH);
    cJSON_AddNumberToObject(result, "height", UI_SCREENSHOT_HEIGHT);
    cJSON_AddStringToObject(result, "format", "rgb565");
    cJSON_AddNumberToObject(result, "total", (double)total);
    cJSON_AddNumberToObject(result, "offset", offset);
    cJSON_AddStringToObject(result, "data", b64);
    cJSON_AddNumberToObject(result, "len", (double)chunk_len);
    free(b64);
    return 0;
#endif
}

/* bramble.injectInput, params: one of
 *   {"type":"trackball","dir":"up"|"down"|"left"|"right"|"select"}
 *   {"type":"key","char":"a"}
 *   {"type":"text","text":"hello","enter":bool}
 * Injects through the same ring the real trackball/keyboard drivers feed
 * (see trackball_inject/keyboard_inject_char), so LVGL group/focus/textarea
 * behavior is identical to physical input. Touch is out of scope. */
static int handle_inject_input(const cJSON* params, cJSON* result) {
#ifndef CONFIG_BRAMBLE_UI_GRAPHICAL
    (void)params;
    cJSON_AddStringToObject(result, "error", "no graphical ui");
    return 0;
#else
    if (!params)
        return RPC_ERR_INVALID_PARAMS;
    const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(params, "type"));
    if (!type)
        return RPC_ERR_INVALID_PARAMS;

    int queued = 0;

    if (strcmp(type, "trackball") == 0) {
        const char* dir = cJSON_GetStringValue(cJSON_GetObjectItem(params, "dir"));
        if (!dir)
            return RPC_ERR_INVALID_PARAMS;

        ui_button_t btn;
        if (strcmp(dir, "up") == 0) {
            btn = BTN_UP;
        } else if (strcmp(dir, "down") == 0) {
            btn = BTN_DOWN;
        } else if (strcmp(dir, "left") == 0) {
            btn = BTN_LEFT;
        } else if (strcmp(dir, "right") == 0) {
            btn = BTN_RIGHT;
        } else if (strcmp(dir, "select") == 0) {
            btn = BTN_SELECT;
        } else {
            return RPC_ERR_INVALID_PARAMS;
        }

        if (trackball_inject(btn))
            queued = 1;
    } else if (strcmp(type, "key") == 0) {
        const char* ch = cJSON_GetStringValue(cJSON_GetObjectItem(params, "char"));
        if (!ch || ch[0] == '\0')
            return RPC_ERR_INVALID_PARAMS;

        if (keyboard_inject_char(ch[0]))
            queued = 1;
    } else if (strcmp(type, "text") == 0) {
        const char* text = cJSON_GetStringValue(cJSON_GetObjectItem(params, "text"));
        if (!text)
            return RPC_ERR_INVALID_PARAMS;

        for (const char* p = text; *p != '\0'; p++) {
            if (keyboard_inject_char(*p))
                queued++;
        }

        cJSON* enter_j = cJSON_GetObjectItem(params, "enter");
        if (cJSON_IsTrue(enter_j) && keyboard_inject_char('\n'))
            queued++;
    } else {
        return RPC_ERR_INVALID_PARAMS;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "queued", queued);
    return 0;
#endif
}

/* ── Traffic debug RPC methods ─────────────────────────────────────── */

/* bramble.setTrafficDebug: params: {"enabled":bool, "include_tx":bool, "include_rx":bool,
 * "sample_rate":0-100} */
static int handle_set_traffic_debug(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;

    /* Default values */
    bool enabled = false;
    bool include_tx = true;
    bool include_rx = true;
    uint8_t sample_rate = 100;

    cJSON* en = cJSON_GetObjectItem(params, "enabled");
    if (en && cJSON_IsBool(en))
        enabled = cJSON_IsTrue(en);

    cJSON* tx = cJSON_GetObjectItem(params, "include_tx");
    if (tx && cJSON_IsBool(tx))
        include_tx = cJSON_IsTrue(tx);

    cJSON* rx = cJSON_GetObjectItem(params, "include_rx");
    if (rx && cJSON_IsBool(rx))
        include_rx = cJSON_IsTrue(rx);

    cJSON* sr = cJSON_GetObjectItem(params, "sample_rate");
    if (sr && cJSON_IsNumber(sr)) {
        int val = sr->valueint;
        sample_rate = (val < 0) ? 0 : (val > 100 ? 100 : (uint8_t)val);
    }

    mesh_traffic_debug_set_config(enabled, include_tx, include_rx, sample_rate);

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddBoolToObject(result, "enabled", enabled);
    cJSON_AddBoolToObject(result, "include_tx", include_tx);
    cJSON_AddBoolToObject(result, "include_rx", include_rx);
    cJSON_AddNumberToObject(result, "sample_rate", sample_rate);
    return 0;
}

/* bramble.getTrafficDebug: returns current config + buffer state */
static int handle_get_traffic_debug(const cJSON* params, cJSON* result) {
    (void)params;

    bool enabled, include_tx, include_rx;
    uint8_t sample_rate;
    mesh_traffic_debug_get_config(&enabled, &include_tx, &include_rx, &sample_rate);

    traffic_debug_t* td = mesh_get_traffic_debug();

    cJSON_AddBoolToObject(result, "enabled", enabled);
    cJSON_AddBoolToObject(result, "include_tx", include_tx);
    cJSON_AddBoolToObject(result, "include_rx", include_rx);
    cJSON_AddNumberToObject(result, "sample_rate", sample_rate);
    cJSON_AddNumberToObject(result, "buffer_capacity", 512);
    cJSON_AddNumberToObject(result, "buffer_count", traffic_debug_get_count(td));
    cJSON_AddNumberToObject(result, "dropped_count", traffic_debug_get_dropped(td));

    return 0;
}

/* bramble.getTrafficEvents: params: {"since_seq":uint32, "limit":uint16} */
static int handle_get_traffic_events(const cJSON* params, cJSON* result) {
    traffic_debug_t* td = mesh_get_traffic_debug();

    uint32_t since_seq = 0;
    uint16_t limit = 100; /* default limit */

    if (params) {
        cJSON* seq = cJSON_GetObjectItem(params, "since_seq");
        if (seq && cJSON_IsNumber(seq))
            since_seq = (uint32_t)seq->valuedouble;

        cJSON* lim = cJSON_GetObjectItem(params, "limit");
        if (lim && cJSON_IsNumber(lim)) {
            int val = lim->valueint;
            limit = (val <= 0) ? 100 : (val > 512 ? 512 : (uint16_t)val);
        }
    }

    cJSON* events = cJSON_AddArrayToObject(result, "events");

    uint16_t count = traffic_debug_get_count(td);
    uint16_t returned = 0;

    for (uint16_t i = 0; i < count && returned < limit; i++) {
        const traffic_event_t* evt = traffic_debug_get_event(td, i);
        if (!evt)
            break;

        /* Filter by seq if requested */
        if (since_seq > 0 && evt->seq <= since_seq)
            continue;

        cJSON* obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "seq", evt->seq);
        cJSON_AddNumberToObject(obj, "timestamp_ms", evt->timestamp_ms);
        cJSON_AddNumberToObject(obj, "pkt_type", evt->pkt_type);

        /* Category and airtime tier as canonical strings (shared with the
         * traffic_event notification serializer via the traffic_debug
         * component). */
        cJSON_AddStringToObject(obj, "category", traffic_debug_category_name(evt->category));
        cJSON_AddStringToObject(obj, "airtime_tier",
                                traffic_debug_airtime_tier_name(evt->airtime_tier));

        cJSON_AddNumberToObject(obj, "packet_len", evt->packet_len);
        cJSON_AddNumberToObject(obj, "rssi", evt->rssi);
        cJSON_AddBoolToObject(obj, "is_tx", evt->is_tx);

        cJSON_AddItemToArray(events, obj);
        returned++;
    }

    cJSON_AddNumberToObject(result, "returned", returned);
    cJSON_AddNumberToObject(result, "total_available", count);

    return 0;
}

/* bramble.setBeaconPolicy: params: {"enabled":bool, "mode":str, "baseIntervalMs":num, ...} */
static int handle_set_beacon_policy(const cJSON* params, cJSON* result) {
    if (!params)
        return RPC_ERR_INVALID_PARAMS;

    beacon_policy_config_t config;
    mesh_get_beacon_policy(&config); /* Start with current config */

    /* Parse parameters */
    cJSON* enabled = cJSON_GetObjectItem(params, "enabled");
    cJSON* mode_str = cJSON_GetObjectItem(params, "mode");
    cJSON* base_ms = cJSON_GetObjectItem(params, "baseIntervalMs");
    cJSON* min_ms = cJSON_GetObjectItem(params, "minIntervalMs");
    cJSON* max_ms = cJSON_GetObjectItem(params, "maxIntervalMs");
    cJSON* dense_th = cJSON_GetObjectItem(params, "denseThreshold");
    cJSON* churn_th = cJSON_GetObjectItem(params, "churnThreshold");
    cJSON* churn_win = cJSON_GetObjectItem(params, "churnWindowMs");

    if (enabled && cJSON_IsBool(enabled)) {
        config.enabled = cJSON_IsTrue(enabled);
    }

    if (mode_str && cJSON_IsString(mode_str)) {
        const char* mode = cJSON_GetStringValue(mode_str);
        if (strcmp(mode, "fixed") == 0) {
            config.mode = BEACON_MODE_FIXED;
        } else if (strcmp(mode, "adaptive") == 0) {
            config.mode = BEACON_MODE_ADAPTIVE;
        } else {
            cJSON_AddStringToObject(result, "error", "Invalid mode (use 'fixed' or 'adaptive')");
            return RPC_ERR_INVALID_PARAMS;
        }
    }

    if (base_ms && cJSON_IsNumber(base_ms)) {
        config.base_interval_ms = (uint32_t)base_ms->valuedouble;
    }
    if (min_ms && cJSON_IsNumber(min_ms)) {
        config.min_interval_ms = (uint32_t)min_ms->valuedouble;
    }
    if (max_ms && cJSON_IsNumber(max_ms)) {
        config.max_interval_ms = (uint32_t)max_ms->valuedouble;
    }
    if (dense_th && cJSON_IsNumber(dense_th)) {
        config.dense_threshold = (uint8_t)dense_th->valueint;
    }
    if (churn_th && cJSON_IsNumber(churn_th)) {
        config.churn_threshold = (uint8_t)churn_th->valueint;
    }
    if (churn_win && cJSON_IsNumber(churn_win)) {
        config.churn_window_ms = (uint32_t)churn_win->valuedouble;
    }

    /* Apply the configuration */
    int rc = mesh_set_beacon_policy(&config);
    if (rc != 0) {
        cJSON_AddStringToObject(result, "error", "Failed to set beacon policy");
        return RPC_ERR_INTERNAL;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

/* bramble.getBeaconPolicy: returns config and status */
static int handle_get_beacon_policy(const cJSON* params, cJSON* result) {
    (void)params;

    beacon_policy_config_t config;
    beacon_policy_status_t status;

    mesh_get_beacon_policy(&config);
    mesh_get_beacon_status(&status);

    /* Config */
    cJSON* cfg = cJSON_AddObjectToObject(result, "config");
    cJSON_AddBoolToObject(cfg, "enabled", config.enabled);
    cJSON_AddStringToObject(cfg, "mode", config.mode == BEACON_MODE_FIXED ? "fixed" : "adaptive");
    cJSON_AddNumberToObject(cfg, "baseIntervalMs", config.base_interval_ms);
    cJSON_AddNumberToObject(cfg, "minIntervalMs", config.min_interval_ms);
    cJSON_AddNumberToObject(cfg, "maxIntervalMs", config.max_interval_ms);
    cJSON_AddNumberToObject(cfg, "denseThreshold", config.dense_threshold);
    cJSON_AddNumberToObject(cfg, "churnThreshold", config.churn_threshold);
    cJSON_AddNumberToObject(cfg, "churnWindowMs", config.churn_window_ms);

    /* Status */
    cJSON* st = cJSON_AddObjectToObject(result, "status");
    cJSON_AddStringToObject(st, "activeMode",
                            status.active_mode == BEACON_MODE_FIXED ? "fixed" : "adaptive");
    cJSON_AddNumberToObject(st, "currentIntervalMs", status.current_interval_ms);
    cJSON_AddNumberToObject(st, "neighborCount", status.neighbor_count);
    cJSON_AddNumberToObject(st, "churnEvents", status.churn_events);
    cJSON_AddNumberToObject(st, "lastTransitionMs", status.last_transition_ms);
    cJSON_AddBoolToObject(st, "inBackoff", status.in_backoff);

    return 0;
}

/* ── PHY passthrough (hardware bridge, DESIGN.md section 10) ─────────── */

/* Emit hook registered with phy_passthrough: serialize one received frame as
 * a bramble.onPhyFrame notification (hex frame + rssi/snr/freq) onto every
 * notification transport. The WS transport applies the authenticated-only
 * notification filter (rpc_auth); the serial transport is trusted by physical
 * access, which is the gateway path. Hex (not base64) matches the rest of the
 * RPC surface's binary encoding and keeps the firmware free of a base64 dep. */
static void phy_emit_frame_notify(const uint8_t* data, uint8_t len, const radio_rx_info_t* info,
                                  uint32_t freq_hz) {
    static const char hexd[] = "0123456789abcdef";
    char frame_hex[2 * 255 + 1];
    for (uint8_t i = 0; i < len; i++) {
        frame_hex[i * 2] = hexd[(data[i] >> 4) & 0xF];
        frame_hex[i * 2 + 1] = hexd[data[i] & 0xF];
    }
    frame_hex[len * 2] = '\0';

    cJSON* params = cJSON_CreateObject();
    if (!params) {
        return;
    }
    cJSON_AddStringToObject(params, "frame", frame_hex);
    cJSON_AddNumberToObject(params, "rssi", info->rssi);
    cJSON_AddNumberToObject(params, "snr", info->snr);
    cJSON_AddNumberToObject(params, "freq", (double)freq_hz);
    rpc_notify("bramble.onPhyFrame", params);
    cJSON_Delete(params);
}

/* Whether this node holds a live channel identity (DESIGN.md section 10): a
 * provisioned network key OR any channel beyond the always-present public
 * channel (index 0) makes it a real mesh participant, so passthrough refuses
 * without force. A bare, unprovisioned sacrificial gateway holds neither and
 * enables cleanly. */
static bool phy_has_live_identity(void) {
    return network_key_is_provisioned() || mesh_get_channel_count() > 1;
}

/* Auto-expiry (the TTL-elapsed live->off transition) happens inside the gate
 * module, which has no logging dependency by design. The module latches that
 * transition; drain and log it here so every state transition is logged
 * (DESIGN.md section 10) exactly once, never per poll. */
static void phy_log_if_auto_expired(void) {
    if (phy_passthrough_consume_auto_expired()) {
        ESP_LOGW(TAG, "PHY passthrough AUTO-EXPIRED (TTL elapsed), now DISABLED");
    }
}

static void phy_add_status(cJSON* result) {
    phy_passthrough_status_t st;
    phy_passthrough_get_status(&st);
    phy_log_if_auto_expired();
    cJSON_AddBoolToObject(result, "enabled", st.active);
    cJSON_AddBoolToObject(result, "forced", st.forced);
    cJSON_AddNumberToObject(result, "ttl_s", st.ttl_s);
    cJSON_AddNumberToObject(result, "remaining_s", st.remaining_s);
}

/* phy.enable, params: {"ttl_s"?: number, "force"?: bool}. Authenticated only
 * (not on rpc_auth's unauth allowlist); over serial the CLI is authenticated
 * by physical access. */
static int handle_phy_enable(const cJSON* params, cJSON* result) {
    uint32_t ttl_s = 0; /* 0 => module default (30 min) */
    bool force = false;
    if (params) {
        const cJSON* ttl_j = cJSON_GetObjectItem(params, "ttl_s");
        if (cJSON_IsNumber(ttl_j) && ttl_j->valuedouble > 0) {
            ttl_s = (uint32_t)ttl_j->valuedouble;
        }
        force = cJSON_IsTrue(cJSON_GetObjectItem(params, "force"));
    }

    int rc = phy_passthrough_enable(ttl_s, force, phy_has_live_identity());
    if (rc == PHY_PT_ERR_IDENTITY) {
        ESP_LOGW(TAG, "PHY passthrough enable REFUSED: node holds a live channel identity "
                      "(force required)");
        cJSON_AddBoolToObject(result, "enabled", false);
        cJSON_AddBoolToObject(result, "requires_force", true);
        cJSON_AddStringToObject(result, "error",
                                "node holds a live channel identity; pass force:true to override");
        return 0;
    }
    /* Log the effective (normalized/clamped) TTL, not the raw param: a default
     * enable arrives as ttl_s=0 but the applied window is PHY_PT_DEFAULT_TTL_S. */
    phy_passthrough_status_t enabled_st;
    phy_passthrough_get_status(&enabled_st);
    ESP_LOGW(TAG, "PHY passthrough ENABLED (ttl_s=%" PRIu32 ", force=%d)", enabled_st.ttl_s,
             (int)force);
    phy_add_status(result);
    return 0;
}

/* phy.disable, no params. */
static int handle_phy_disable(const cJSON* params, cJSON* result) {
    (void)params;
    phy_passthrough_disable();
    ESP_LOGW(TAG, "PHY passthrough DISABLED");
    phy_add_status(result);
    return 0;
}

/* phy.status, no params. */
static int handle_phy_status(const cJSON* params, cJSON* result) {
    (void)params;
    phy_add_status(result);
    return 0;
}

/* phy.tx, params: {"frame": "<hex>"}. Transmits a raw frame on the real
 * channel. Refuses unless passthrough is active. The frame STILL goes through
 * tx_gate (budget check -> LBT -> transmit -> ToA debit): airtime accounting
 * applies to the physical channel even in passthrough, so a bridged gateway
 * cannot flood the air unbudgeted. */
static int handle_phy_tx(const cJSON* params, cJSON* result) {
    if (!phy_passthrough_is_active()) {
        /* is_active() may have just folded a TTL elapse into a disable; log that
         * live->off transition (once) before refusing the TX. */
        phy_log_if_auto_expired();
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "passthrough not active");
        return 0;
    }
    const char* hex = params ? cJSON_GetStringValue(cJSON_GetObjectItem(params, "frame")) : NULL;
    if (!hex) {
        return RPC_ERR_INVALID_PARAMS;
    }
    size_t hlen = strlen(hex);
    if (hlen == 0 || (hlen % 2) != 0 || hlen > 2 * 255) {
        return RPC_ERR_INVALID_PARAMS;
    }
    uint8_t frame[255];
    size_t n = hlen / 2;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return RPC_ERR_INVALID_PARAMS;
        }
        frame[i] = (uint8_t)((hi << 4) | lo);
    }

    int rc = tx_gate_send(frame, (uint8_t)n, TX_KIND_FORWARD);
    if (rc == TX_GATE_OK) {
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddNumberToObject(result, "len", (double)n);
        return 0;
    }
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "error",
                            rc == TX_GATE_ERR_BUDGET ? "airtime budget denied" : "radio error");
    return 0;
}

void rpc_methods_init(bramble_identity_t* identity) {
    s_identity = identity;

    /* PHY passthrough (hardware bridge) forwards received frames through this
     * notification hook; the gate itself is disabled at boot (module state, no
     * NVS, so it never survives a reboot). */
    phy_passthrough_set_emit(phy_emit_frame_notify);

    /* Query methods */
    rpc_register("bramble.getStatus", handle_get_status);
    rpc_register("bramble.getDiagnostics", handle_get_diagnostics);
    rpc_register("bramble.getWifiStatus", handle_get_wifi_status);
    rpc_register("bramble.setWifiConfig", handle_set_wifi_config);
    rpc_register("bramble.getIdentity", handle_get_identity);
    rpc_register("bramble.getPeerVerification", handle_get_peer_verification);
    rpc_register("bramble.getVersion", handle_get_version);
    rpc_register("bramble.getDeliveryEvents", handle_get_delivery_events);
    rpc_register("bramble.getNeighbors", handle_get_neighbors);
    rpc_register("bramble.getRoutes", handle_get_routes);
    rpc_register("bramble.getAirtime", handle_get_airtime);
    rpc_register("bramble.ping", handle_ping);
    rpc_register("bramble.getConfig", handle_get_config);
    rpc_register("bramble.getMessages", handle_get_messages);
    rpc_register("bramble.getPeerLocations", handle_get_peer_locations);

    /* Action methods */
    rpc_register("bramble.sendMessage", handle_send_message);
    rpc_register("bramble.sendBroadcast", handle_send_broadcast);
    rpc_register("bramble.reboot", handle_reboot);
    rpc_register("bramble.enterDfu", handle_enter_dfu);
    rpc_register("bramble.sendProbe", handle_send_probe);
    rpc_register("bramble.setRadio", handle_set_radio);
    rpc_register("bramble.setNodeName", handle_set_node_name);
    rpc_register("bramble.setPeerVerified", handle_set_peer_verified);
    rpc_register("bramble.setAuthToken", rpc_set_auth_token);
    rpc_register("bramble.getAuthToken", rpc_get_auth_token);
    rpc_register("bramble.setNetworkKey", rpc_set_network_key);
    rpc_register("bramble.generateNetworkKey", handle_generate_network_key);
    rpc_register("bramble.getNetworkKeyStatus", handle_get_network_key_status);
    rpc_register("bramble.setAnchor", rpc_set_anchor);
    rpc_register("bramble.getAnchorStatus", handle_get_anchor_status);
    rpc_register("bramble.setEndorsement", rpc_set_endorsement);
    rpc_register("bramble.setAllowedOrigins", rpc_set_allowed_origins);
    rpc_register("bramble.getAllowedOrigins", rpc_get_allowed_origins);
    rpc_register("bramble.addChannel", handle_add_channel);
    rpc_register("bramble.removeChannel", handle_remove_channel);
    rpc_register("bramble.setDefaultChannel", handle_set_default_channel);
    rpc_register("bramble.setMailbox", handle_set_mailbox);
    rpc_register("bramble.setFloodTransport", handle_set_flood_transport);
    rpc_register("bramble.setFloodHopLimit", handle_set_flood_hop_limit);
    rpc_register("bramble.setBroadcastTelemetryMode", handle_set_broadcast_telemetry_mode);
    rpc_register("bramble.setLocationConfig", handle_set_location_config);
    rpc_register("bramble.setLocationContact", handle_set_location_contact);
    rpc_register("bramble.removeLocationContact", handle_remove_location_contact);
    rpc_register("bramble.shareLocationOnce", handle_share_location_once);
    rpc_register("bramble.otaUpdate", handle_ota_update);
    rpc_register("bramble.otaGetOrigin", handle_ota_get_origin);
    rpc_register("bramble.otaSetOrigin", handle_ota_set_origin);
    rpc_register("bramble.otaStatus", handle_ota_status);
    rpc_register("bramble.getBattery", handle_get_battery);
    rpc_register("bramble.setBacklight", handle_set_backlight);
    rpc_register("bramble.sleep", handle_sleep);

    rpc_register("bramble.getGpsPosition", handle_get_gps_position);
    rpc_register("bramble.setGpsEnabled", handle_set_gps_enabled);

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    rpc_register("bramble.getStorageInfo", handle_get_storage_info);
    rpc_register("bramble.playTone", handle_play_tone);
    rpc_register("bramble.setVolume", handle_set_volume);
    rpc_register("bramble.setMuted", handle_set_muted);
    rpc_register("bramble.getAudioStatus", handle_get_audio_status);
#endif

    /* Bench debug methods (screenshot + input injection). Registered on
     * every board; not on the unauth allowlist. See rpc_auth.h. */
    rpc_register("bramble.screenshot", handle_screenshot);
    rpc_register("bramble.injectInput", handle_inject_input);

    /* Traffic debug methods */
    rpc_register("bramble.setTrafficDebug", handle_set_traffic_debug);
    rpc_register("bramble.getTrafficDebug", handle_get_traffic_debug);
    rpc_register("bramble.getTrafficEvents", handle_get_traffic_events);

    /* Beacon policy methods */
    rpc_register("bramble.setBeaconPolicy", handle_set_beacon_policy);
    rpc_register("bramble.getBeaconPolicy", handle_get_beacon_policy);

    /* PHY passthrough (hardware bridge, DESIGN.md section 10). Registered
     * normally, so they are authenticated-only (not on the unauth allowlist). */
    rpc_register("phy.enable", handle_phy_enable);
    rpc_register("phy.disable", handle_phy_disable);
    rpc_register("phy.status", handle_phy_status);
    rpc_register("phy.tx", handle_phy_tx);

    ota_progress_set_callback(ota_progress_notify_cb);

    ESP_LOGI(TAG, "RPC methods registered");
}
