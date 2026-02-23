#include "rpc_methods.h"
#include "rpc_dispatcher.h"
#include "mesh_task.h"
#include "msg_store.h"
#include "airtime_budget.h"
#include "radio.h"
#include "freq_plan.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "battery.h"
#include "ota.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "board_config.h"
#include "display.h"

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#include "audio.h"
#endif

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
/* statvfs not available in ESP-IDF newlib */

#define BRAMBLE_VERSION_STR      "0.2.0-dev"
#define BRAMBLE_PROTOCOL_VERSION "0.2.0"

#define NVS_NAMESPACE            "bramble"
#define NVS_KEY_NODE_NAME        "node_name"

static const char *TAG = "rpc_methods";
static bramble_identity_t *s_identity;

static const char *bramble_hardware(void) {
    const bramble_board_config_t *board = board_get_config();
    if (board && board->short_name && board->short_name[0] != '\0') {
        return board->short_name;
    }
    return "unknown";
}

/* ── Utility ────────────────────────────────────────────────────────── */

static const char *addr_hex(uint32_t addr, char *buf, size_t len) {
    snprintf(buf, len, "%08" PRIX32, addr);
    return buf;
}

/* ── Query handlers (pre-existing) ─────────────────────────────────── */

/* bramble.getStatus */
static int handle_get_status(const cJSON *params, cJSON *result) {
    (void)params;
    char buf[12];
    static mesh_shared_state_t st;
    mesh_get_state(&st);

    cJSON_AddStringToObject(result, "address", addr_hex(s_identity->address, buf, sizeof(buf)));
    cJSON_AddStringToObject(result, "firmware_version", BRAMBLE_VERSION_STR);
    cJSON_AddStringToObject(result, "protocol_version", BRAMBLE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(result, "hardware", bramble_hardware());
    cJSON_AddBoolToObject(result, "radio_ok", st.radio_ok);
    cJSON_AddNumberToObject(result, "peers", st.neighbors.count);
    cJSON_AddNumberToObject(result, "beacon_tx", st.beacon_tx_count);
    cJSON_AddNumberToObject(result, "beacon_rx", st.beacon_rx_count);
    cJSON_AddNumberToObject(result, "packets_tx", st.packets_tx);
    cJSON_AddNumberToObject(result, "packets_rx", st.packets_rx);
    cJSON_AddNumberToObject(result, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(result, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(result, "battery_mv", battery_read_mv());
    cJSON_AddNumberToObject(result, "battery_pct", battery_read_pct());
#ifdef CONFIG_BRAMBLE_HAS_GPS
    cJSON_AddBoolToObject(result, "gps_available", true);
#else
    cJSON_AddBoolToObject(result, "gps_available", false);
#endif
    return 0;
}

/* bramble.getIdentity */
static int handle_get_identity(const cJSON *params, cJSON *result) {
    (void)params;
    char buf[12];
    cJSON_AddStringToObject(result, "address", addr_hex(s_identity->address, buf, sizeof(buf)));
    cJSON_AddStringToObject(result, "pubkey_hash", addr_hex(s_identity->pubkey_hash, buf, sizeof(buf)));
    return 0;
}

/* bramble.getVersion */
static int handle_get_version(const cJSON *params, cJSON *result) {
    (void)params;
    cJSON_AddStringToObject(result, "firmware_version", BRAMBLE_VERSION_STR);
    cJSON_AddStringToObject(result, "protocol_version", BRAMBLE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(result, "hardware", bramble_hardware());
    return 0;
}

/* bramble.getNeighbors */
static int handle_get_neighbors(const cJSON *params, cJSON *result) {
    (void)params;
    static mesh_shared_state_t st;
    mesh_get_state(&st);

    cJSON *arr = cJSON_AddArrayToObject(result, "neighbors");
    char buf[12];

    for (int i = 0; i < st.neighbors.count; i++) {
        const neighbor_entry_t *n = &st.neighbors.entries[i];
        if (n->addr == 0) continue;

        cJSON *obj = cJSON_CreateObject();
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
    return 0;
}

/* bramble.getRoutes */
static int handle_get_routes(const cJSON *params, cJSON *result) {
    (void)params;
    routing_table_t routes;
    mesh_get_routes(&routes);

    cJSON *arr = cJSON_AddArrayToObject(result, "routes");
    char buf[12];
    static const char *state_names[] = {
        "discovering", "unverified", "active", "stale", "broken"
    };
    for (int i = 0; i < routes.count; i++) {
        const route_entry_t *r = &routes.entries[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "dest", addr_hex(r->dest_addr, buf, sizeof(buf)));
        cJSON_AddStringToObject(obj, "next_hop", addr_hex(r->next_hop, buf, sizeof(buf)));
        cJSON_AddNumberToObject(obj, "hop_count", r->hop_count);
        cJSON_AddNumberToObject(obj, "metric", r->metric);
        cJSON_AddStringToObject(obj, "state",
            r->state <= ROUTE_BROKEN ? state_names[r->state] : "unknown");
        cJSON_AddNumberToObject(obj, "use_count", r->use_count);
        cJSON_AddItemToArray(arr, obj);
    }
    return 0;
}

/* bramble.getAirtime */
static int handle_get_airtime(const cJSON *params, cJSON *result) {
    (void)params;
    static mesh_shared_state_t st;
    mesh_get_state(&st);
    /* Refill before reporting so values are current */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    airtime_budget_refill(&st.airtime, now_ms);
    cJSON_AddNumberToObject(result, "critical_remaining_ms", airtime_budget_remaining(&st.airtime, AIRTIME_TIER_CRITICAL));
    cJSON_AddNumberToObject(result, "normal_remaining_ms", airtime_budget_remaining(&st.airtime, AIRTIME_TIER_NORMAL));
    cJSON_AddNumberToObject(result, "broadcast_remaining_ms", airtime_budget_remaining(&st.airtime, AIRTIME_TIER_BROADCAST));
    cJSON_AddNumberToObject(result, "critical_max_ms", AIRTIME_BUDGET_CRITICAL_MS);
    cJSON_AddNumberToObject(result, "normal_max_ms", AIRTIME_BUDGET_NORMAL_MS);
    cJSON_AddNumberToObject(result, "broadcast_max_ms", AIRTIME_BUDGET_BROADCAST_MS);
    cJSON_AddNumberToObject(result, "next_refill_ms", airtime_budget_next_refill_ms(&st.airtime, now_ms));
    return 0;
}

/* bramble.ping */
static int handle_ping(const cJSON *params, cJSON *result) {
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
#define SINGLE_PACKET_MAX_BYTES    203
/* Fragment payload per packet: 154 bytes (from fragment.h FRAG_MAX_PLAINTEXT) */
#define FRAGMENT_PAYLOAD_BYTES     154
/* Max fragments per message: 4 (from fragment.h FRAG_MAX_FRAGMENTS) */
#define MAX_FRAGMENTS              4
/* True max with fragmentation: 154 * 4 = 616 bytes */
#define FRAGMENTED_MAX_BYTES       (FRAGMENT_PAYLOAD_BYTES * MAX_FRAGMENTS)

/* bramble.sendMessage — params: {"dest":"HEXADDR", "text":"...", "channel"?:N} */
static int handle_send_message(const cJSON *params, cJSON *result) {
    const char *dest_str = cJSON_GetStringValue(cJSON_GetObjectItem(params, "dest"));
    const char *text     = cJSON_GetStringValue(cJSON_GetObjectItem(params, "text"));
    cJSON *channel_j     = cJSON_GetObjectItem(params, "channel");
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

        pkt_id = mesh_send_channel(ch, dest, (const uint8_t *)text, text_len);
        if (pkt_id == 0) {
            ESP_LOGW(TAG, "mesh_send_channel ch=%d to %08" PRIX32 " failed", ch, dest);
            cJSON_AddStringToObject(result, "error", "send failed");
            return RPC_ERR_RADIO;
        }
        cJSON_AddNumberToObject(result, "channel", ch);
    } else {
        pkt_id = mesh_send_message(dest, (const uint8_t *)text, text_len);
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

/* bramble.sendBroadcast — params: {"text":"..."} */
static int handle_send_broadcast(const cJSON *params, cJSON *result) {
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(params, "text"));
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

    int rc = mesh_send_broadcast((const uint8_t *)text, text_len);
    if (rc == -2) {
        cJSON_AddStringToObject(result, "error", "rate limited");
        return RPC_ERR_RATE_LIMIT;
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "mesh_send_broadcast failed: %d", rc);
        cJSON_AddStringToObject(result, "error", "send failed");
        return RPC_ERR_RADIO;
    }

    /* Generate message ID from incrementing counter (broadcast packets don't return packet_id) */
    static uint32_t broadcast_msg_counter = 1;
    char msg_id[12];
    snprintf(msg_id, sizeof(msg_id), "B%07" PRIu32, broadcast_msg_counter++);
    cJSON_AddStringToObject(result, "message_id", msg_id);
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

/* bramble.reboot — no params required */
static int handle_reboot(const cJSON *params, cJSON *result) {
    (void)params;
    cJSON_AddBoolToObject(result, "ok", true);
    mesh_reboot_delayed(500);
    return 0;
}

/* bramble.sendProbe — stub: params {"dest":"HEXADDR"} */
static int handle_send_probe(const cJSON *params, cJSON *result) {
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

/* bramble.setRadio — stub: params {"sf":9, "bw_hz":125000, "tx_power":17, "freq_mhz":915.0} */
static int handle_set_radio(const cJSON *params, cJSON *result) {
    if (!params) return RPC_ERR_INVALID_PARAMS;

    /* Get current config as base */
    radio_config_t cfg;
    radio_get_config(&cfg);

    /* Apply any provided fields */
    cJSON *freq = cJSON_GetObjectItem(params, "frequency_mhz");
    cJSON *sf   = cJSON_GetObjectItem(params, "sf");
    cJSON *bw   = cJSON_GetObjectItem(params, "bw_hz");
    cJSON *txp  = cJSON_GetObjectItem(params, "tx_power_dbm");
    cJSON *cr   = cJSON_GetObjectItem(params, "coding_rate");

    if (freq && cJSON_IsNumber(freq)) cfg.frequency_mhz = (float)freq->valuedouble;
    if (sf   && cJSON_IsNumber(sf))   cfg.sf = (uint8_t)sf->valueint;
    if (bw   && cJSON_IsNumber(bw))   cfg.bw_hz = (uint32_t)bw->valuedouble;
    if (txp  && cJSON_IsNumber(txp))  cfg.tx_power = (int8_t)txp->valueint;
    if (cr   && cJSON_IsNumber(cr))   cfg.coding_rate = (uint8_t)cr->valueint;

    /* Validate against freq plan */
    const bramble_freq_plan_t *plan = freq_plan_get_default();
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
    esp_err_t err = nvs_open("bramble_radio", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        /* Store as integers to avoid float NVS issues */
        nvs_set_u32(nvs, "freq_khz", (uint32_t)(cfg.frequency_mhz * 1000));
        nvs_set_u8(nvs, "sf", cfg.sf);
        nvs_set_u32(nvs, "bw_hz", cfg.bw_hz);
        nvs_set_i8(nvs, "tx_power", cfg.tx_power);
        nvs_set_u8(nvs, "cr", cfg.coding_rate);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Radio reconfigured: %.1f MHz SF%u BW%" PRIu32 " TX %ddBm",
             cfg.frequency_mhz, cfg.sf, cfg.bw_hz, cfg.tx_power);

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "frequency_mhz", cfg.frequency_mhz);
    cJSON_AddNumberToObject(result, "sf", cfg.sf);
    cJSON_AddNumberToObject(result, "bw_hz", cfg.bw_hz);
    cJSON_AddNumberToObject(result, "tx_power_dbm", cfg.tx_power);
    cJSON_AddNumberToObject(result, "coding_rate", cfg.coding_rate);
    return 0;
}

/* bramble.setNodeName — params: {"name":"..."} — persists to NVS */
static int handle_set_node_name(const cJSON *params, cJSON *result) {
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(params, "name"));
    if (!name || strlen(name) == 0 || strlen(name) > 32) {
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

    ESP_LOGI(TAG, "Node name set to: %s", name);
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "name", name);
    return 0;
}

/* bramble.addChannel — params {"name":"...", "psk":"hexkey"} */
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int handle_add_channel(const cJSON *params, cJSON *result) {
    if (!params) return RPC_ERR_INVALID_PARAMS;
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(params, "name"));
    const char *psk = cJSON_GetStringValue(cJSON_GetObjectItem(params, "psk"));
    if (!name || strlen(name) == 0 || strlen(name) > 19) {
        return RPC_ERR_INVALID_PARAMS;
    }

    uint8_t psk_bytes[32];
    const uint8_t *psk_ptr = NULL;
    size_t psk_len = 0;
    if (psk && psk[0] != '\0') {
        size_t hex_len = strlen(psk);
        if ((hex_len % 2) != 0 || hex_len > sizeof(psk_bytes) * 2) {
            return RPC_ERR_INVALID_PARAMS;
        }
        for (size_t i = 0; i < hex_len; i += 2) {
            int hi = hex_nibble(psk[i]);
            int lo = hex_nibble(psk[i + 1]);
            if (hi < 0 || lo < 0) {
                return RPC_ERR_INVALID_PARAMS;
            }
            psk_bytes[i / 2] = (uint8_t)((hi << 4) | lo);
        }
        psk_ptr = psk_bytes;
        psk_len = hex_len / 2;
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

static int handle_remove_channel(const cJSON *params, cJSON *result) {
    if (!params) return RPC_ERR_INVALID_PARAMS;
    cJSON *idx_j = cJSON_GetObjectItem(params, "index");
    if (!idx_j || !cJSON_IsNumber(idx_j)) return RPC_ERR_INVALID_PARAMS;
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

static int handle_set_default_channel(const cJSON *params, cJSON *result) {
    if (!params) return RPC_ERR_INVALID_PARAMS;
    cJSON *idx_j = cJSON_GetObjectItem(params, "index");
    if (!idx_j || !cJSON_IsNumber(idx_j)) return RPC_ERR_INVALID_PARAMS;

    int rc = mesh_set_default_channel(idx_j->valueint);
    if (rc != 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "invalid channel index");
        return 0;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

static int handle_set_mailbox(const cJSON *params, cJSON *result) {
    if (!params) return RPC_ERR_INVALID_PARAMS;
    cJSON *enabled = cJSON_GetObjectItem(params, "enabled");
    if (!enabled || !cJSON_IsBool(enabled)) return RPC_ERR_INVALID_PARAMS;

    /* Persist to NVS */
    nvs_handle_t nvs;
    if (nvs_open("bramble_mb", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "enabled", cJSON_IsTrue(enabled) ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    bool en = cJSON_IsTrue(enabled);
    mesh_set_mailbox(en);

    ESP_LOGI("rpc", "Mailbox %s", en ? "enabled" : "disabled");
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddBoolToObject(result, "enabled", en);
    return 0;
}

static int handle_set_location_config(const cJSON *params, cJSON *result) {
    if (!params) return RPC_ERR_INVALID_PARAMS;

    nvs_handle_t nvs;
    if (nvs_open("bramble_loc", NVS_READWRITE, &nvs) != ESP_OK) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "NVS open failed");
        return 0;
    }

    cJSON *enabled = cJSON_GetObjectItem(params, "enabled");
    if (enabled && cJSON_IsBool(enabled))
        nvs_set_u8(nvs, "enabled", cJSON_IsTrue(enabled) ? 1 : 0);

    cJSON *interval = cJSON_GetObjectItem(params, "interval_s");
    if (interval && cJSON_IsNumber(interval))
        nvs_set_u16(nvs, "interval_s", (uint16_t)interval->valueint);

    cJSON *tier = cJSON_GetObjectItem(params, "default_tier");
    if (tier && cJSON_IsString(tier))
        nvs_set_str(nvs, "def_tier", tier->valuestring);

    /* Accept manual coordinates (no GPS hardware on Heltec V3) */
    cJSON *lat = cJSON_GetObjectItem(params, "lat");
    cJSON *lon = cJSON_GetObjectItem(params, "lon");
    if (lat && cJSON_IsNumber(lat))
        nvs_set_i32(nvs, "lat_e6", (int32_t)(lat->valuedouble * 1e6));
    if (lon && cJSON_IsNumber(lon))
        nvs_set_i32(nvs, "lon_e6", (int32_t)(lon->valuedouble * 1e6));

    nvs_commit(nvs);
    nvs_close(nvs);

    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

static int handle_set_location_contact(const cJSON *params, cJSON *result) {
    if (!params) return RPC_ERR_INVALID_PARAMS;
    const char *addr_str = cJSON_GetStringValue(cJSON_GetObjectItem(params, "address"));
    const char *tier = cJSON_GetStringValue(cJSON_GetObjectItem(params, "tier"));
    if (!addr_str) return RPC_ERR_INVALID_PARAMS;

    nvs_handle_t nvs;
    if (nvs_open("bramble_loc", NVS_READWRITE, &nvs) != ESP_OK) {
        cJSON_AddBoolToObject(result, "ok", false);
        return 0;
    }

    /* Store contact tier: key = "lc_HEXADDR", value = tier string */
    char key[16];
    snprintf(key, sizeof(key), "lc_%.8s", addr_str);
    nvs_set_str(nvs, key, tier ? tier : "zone");
    nvs_commit(nvs);
    nvs_close(nvs);

    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

static int handle_remove_location_contact(const cJSON *params, cJSON *result) {
    if (!params) return RPC_ERR_INVALID_PARAMS;
    const char *addr_str = cJSON_GetStringValue(cJSON_GetObjectItem(params, "address"));
    if (!addr_str) return RPC_ERR_INVALID_PARAMS;

    nvs_handle_t nvs;
    if (nvs_open("bramble_loc", NVS_READWRITE, &nvs) != ESP_OK) {
        cJSON_AddBoolToObject(result, "ok", false);
        return 0;
    }

    char key[16];
    snprintf(key, sizeof(key), "lc_%.8s", addr_str);
    nvs_erase_key(nvs, key);
    nvs_commit(nvs);
    nvs_close(nvs);

    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}

static int handle_share_location_once(const cJSON *params, cJSON *result) {
    if (!params) return RPC_ERR_INVALID_PARAMS;
    const char *addr_str = cJSON_GetStringValue(cJSON_GetObjectItem(params, "address"));
    if (!addr_str) return RPC_ERR_INVALID_PARAMS;

    /* Read stored location from NVS */
    nvs_handle_t nvs;
    if (nvs_open("bramble_loc", NVS_READONLY, &nvs) != ESP_OK) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "no location configured");
        return 0;
    }
    int32_t lat_e6 = 0, lon_e6 = 0;
    nvs_get_i32(nvs, "lat_e6", &lat_e6);
    nvs_get_i32(nvs, "lon_e6", &lon_e6);
    nvs_close(nvs);

    if (lat_e6 == 0 && lon_e6 == 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "no location set (use setLocationConfig with lat/lon)");
        return 0;
    }

    /* Build location payload as JSON and send as a message */
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"type\":\"location\",\"lat\":%.6f,\"lon\":%.6f}",
             lat_e6 / 1e6, lon_e6 / 1e6);

    /* Parse destination address */
    uint32_t dest_addr = (uint32_t)strtoul(addr_str, NULL, 16);
    uint32_t pkt_id = mesh_send_message(dest_addr,
                                        (const uint8_t *)payload,
                                        strlen(payload));
    if (pkt_id == 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "send failed (no route or radio busy)");
        return 0;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "lat", lat_e6 / 1e6);
    cJSON_AddNumberToObject(result, "lon", lon_e6 / 1e6);
    char pkt_buf[12];
    snprintf(pkt_buf, sizeof(pkt_buf), "%08" PRIX32, pkt_id);
    cJSON_AddStringToObject(result, "packetId", pkt_buf);
    return 0;
}

/* bramble.getMessages — returns stored messages from ring buffer */
static int handle_get_messages(const cJSON *params, cJSON *result) {
    (void)params;
    cJSON *arr = cJSON_AddArrayToObject(result, "messages");
    char buf[12];

    int count = msg_store_count();
    for (int i = 0; i < count; i++) {
        const stored_msg_t *m = msg_store_get(i);
        if (!m) continue;

        cJSON *obj = cJSON_CreateObject();
        char buf2[12];
        bool is_out = (m->direction == MSG_DIR_OUTGOING || m->direction == MSG_DIR_BROADCAST_OUT);
        cJSON_AddStringToObject(obj, "from",
            is_out ? addr_hex(s_identity->address, buf, sizeof(buf))
                   : addr_hex(m->peer_addr, buf, sizeof(buf)));
        cJSON_AddStringToObject(obj, "to",
            is_out ? addr_hex(m->peer_addr, buf2, sizeof(buf2))
                   : addr_hex(s_identity->address, buf2, sizeof(buf2)));

        const char *dir_str = "incoming";
        switch (m->direction) {
            case MSG_DIR_OUTGOING:       dir_str = "outgoing"; break;
            case MSG_DIR_BROADCAST_IN:   dir_str = "broadcast_in"; break;
            case MSG_DIR_BROADCAST_OUT:  dir_str = "broadcast_out"; break;
            default: break;
        }
        cJSON_AddStringToObject(obj, "direction", dir_str);
        cJSON_AddStringToObject(obj, "text", m->text);
        cJSON_AddNumberToObject(obj, "channel", m->channel_index);
        cJSON_AddBoolToObject(obj, "broadcast", (m->direction == MSG_DIR_BROADCAST_IN || m->direction == MSG_DIR_BROADCAST_OUT));
        cJSON_AddNumberToObject(obj, "timestamp_s", m->timestamp_s);
        if (m->rssi != 0) cJSON_AddNumberToObject(obj, "rssi", m->rssi);
        if (m->snr != 0)  cJSON_AddNumberToObject(obj, "snr", m->snr);
        static const char *status_names[] = {"none", "sent", "delivered", "failed"};
        if (m->status > 0 && m->status <= 3) {
            cJSON_AddStringToObject(obj, "status", status_names[m->status]);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    return 0;
}

/* bramble.getPeerLocations — returns own location + any received peer locations */
static int handle_get_peer_locations(const cJSON *params, cJSON *result) {
    (void)params;
    cJSON *peers = cJSON_AddArrayToObject(result, "peers");

    /* Include own location if set */
    nvs_handle_t nvs;
    if (nvs_open("bramble_loc", NVS_READONLY, &nvs) == ESP_OK) {
        int32_t lat_e6 = 0, lon_e6 = 0;
        uint8_t enabled = 0;
        nvs_get_u8(nvs, "enabled", &enabled);
        nvs_get_i32(nvs, "lat_e6", &lat_e6);
        nvs_get_i32(nvs, "lon_e6", &lon_e6);
        nvs_close(nvs);

        if (enabled && (lat_e6 != 0 || lon_e6 != 0)) {
            cJSON *self = cJSON_CreateObject();
            char buf[12];
            cJSON_AddStringToObject(self, "address",
                addr_hex(s_identity->address, buf, sizeof(buf)));
            cJSON_AddNumberToObject(self, "lat", lat_e6 / 1e6);
            cJSON_AddNumberToObject(self, "lon", lon_e6 / 1e6);
            cJSON_AddStringToObject(self, "tier", "exact");
            cJSON_AddBoolToObject(self, "is_self", true);
            cJSON_AddItemToArray(peers, self);
        }
    }

    /* TODO: add received peer locations — pending peer location protocol integration.
     * Location component (components/location) exists with cache API, but mesh_task.c
     * does not yet handle PKT_TYPE_LOCATION packets or maintain a location_manager_t.
     * Once integrated, use location_cache_get() to retrieve peer positions here. */
    return 0;
}

/* bramble.getConfig — returns node name + radio config + channel list */
static int handle_get_config(const cJSON *params, cJSON *result) {
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
    cJSON_AddStringToObject(result, "pubkey_hash", addr_hex(s_identity->pubkey_hash, buf, sizeof(buf)));

    /* Radio config — read actual runtime state */
    radio_config_t rcfg;
    radio_get_config(&rcfg);
    cJSON *radio = cJSON_CreateObject();
    cJSON_AddNumberToObject(radio, "frequency_mhz", rcfg.frequency_mhz);
    cJSON_AddNumberToObject(radio, "sf", rcfg.sf);
    cJSON_AddNumberToObject(radio, "bw_hz", rcfg.bw_hz);
    cJSON_AddNumberToObject(radio, "tx_power_dbm", rcfg.tx_power);
    cJSON_AddNumberToObject(radio, "coding_rate", rcfg.coding_rate);
    cJSON_AddStringToObject(radio, "profile", "custom");
    cJSON_AddItemToObject(result, "radio", radio);

    /* Channel list — read from mesh runtime state */
    int default_channel = 0;
    int ch_count = mesh_get_channel_info(&default_channel);
    cJSON *channels = cJSON_CreateArray();

    for (int i = 0; i < ch_count; i++) {
        cJSON *ch = cJSON_CreateObject();
        const char *ch_name = mesh_get_channel_name(i);
        bool has_psk = false;
        uint16_t epoch = 0;
        mesh_get_channel_security(i, &has_psk, &epoch);

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

    return 0;
}

/* ── Registration ───────────────────────────────────────────────────── */

/* OTA task — runs in background after RPC response */
static char s_ota_url[256];
static void ota_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(500)); /* let RPC response flush */
    int rc = ota_wifi_start(s_ota_url);
    if (rc == 0) {
        ESP_LOGI("ota", "OTA complete — rebooting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    ESP_LOGE("ota", "OTA failed");
    vTaskDelete(NULL);
}

/* bramble.otaUpdate — start OTA from URL */
static int handle_ota_update(const cJSON *params, cJSON *result) {
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(params, "url"));
    if (!url || strlen(url) == 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "url parameter required");
        return 0;
    }

    strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
    s_ota_url[sizeof(s_ota_url) - 1] = '\0';

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "note", "OTA starting — device will reboot on success");
    cJSON_AddStringToObject(result, "partition", ota_get_running_partition());

    xTaskCreate(ota_task, "ota", 8192, NULL, 3, NULL);
    return 0;
}

/* bramble.sleep — enter deep sleep with optional wake timer */
static int handle_sleep(const cJSON *params, cJSON *result) {
    int wake_sec = 0;
    if (params) {
        cJSON *ws = cJSON_GetObjectItem(params, "wake_after_s");
        if (ws && cJSON_IsNumber(ws)) wake_sec = ws->valueint;
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

    if (wake_sec > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)wake_sec * 1000000ULL);
    }

    /* Wake on LoRa DIO1 (GPIO14 on Heltec V3) — any incoming packet */
    esp_sleep_enable_ext0_wakeup(14, 1); /* wake on HIGH */

    esp_deep_sleep_start();
    /* never reached */
    return 0;
}

/* bramble.getBattery — returns battery voltage and percentage */
static int handle_get_battery(const cJSON *params, cJSON *result) {
    (void)params;
    cJSON_AddNumberToObject(result, "voltage_mv", battery_read_mv());
    cJSON_AddNumberToObject(result, "percentage", battery_read_pct());
    return 0;
}

/* bramble.setBacklight — control display backlight */
static int handle_set_backlight(const cJSON *params, cJSON *result) {
    const bramble_board_config_t *board = board_get_config();
    if (board->spi_display.backlight < 0) {
        cJSON_AddStringToObject(result, "error", "no backlight control");
        return -1;
    }
    
    cJSON *level = cJSON_GetObjectItem(params, "level");
    if (!level || !cJSON_IsNumber(level)) {
        return RPC_ERR_INVALID_PARAMS;
    }
    
    int val = level->valueint;
    uint8_t duty = (val <= 0) ? 0 : (val >= 255 ? 255 : (uint8_t)val);
    display_set_backlight(duty);
    cJSON_AddNumberToObject(result, "level", duty);
    return 0;
}

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#include "gps.h"
#include "sdcard.h"

/* bramble.getGpsPosition — returns GPS position if available */
static int handle_get_gps_position(const cJSON *params, cJSON *result) {
    (void)params;
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

/* bramble.getStorageInfo — returns SD card status */
static int handle_get_storage_info(const cJSON *params, cJSON *result) {
    (void)params;
    cJSON_AddBoolToObject(result, "sd_present", sdcard_is_present());
    if (sdcard_is_present()) {
        cJSON_AddStringToObject(result, "mount_point", sdcard_get_mount_point());
        /* Free space reporting: ESP-IDF doesn't expose statvfs, skip for now */
    }
    return 0;
}

/* bramble.playTone — play a predefined alert tone */
static int handle_play_tone(const cJSON *params, cJSON *result) {
    (void)result;
    cJSON *tone = cJSON_GetObjectItem(params, "tone");
    if (!tone || !cJSON_IsString(tone)) {
        return RPC_ERR_INVALID_PARAMS;
    }

    const char *name = tone->valuestring;
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

/* bramble.setVolume — set audio volume 0-100 */
static int handle_set_volume(const cJSON *params, cJSON *result) {
    (void)result;
    cJSON *vol = cJSON_GetObjectItem(params, "volume");
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

/* bramble.setMuted — mute or unmute audio */
static int handle_set_muted(const cJSON *params, cJSON *result) {
    (void)result;
    cJSON *muted = cJSON_GetObjectItem(params, "muted");
    if (!muted || !cJSON_IsBool(muted)) {
        return RPC_ERR_INVALID_PARAMS;
    }
    audio_set_muted(cJSON_IsTrue(muted));
    return 0;
}

/* bramble.getAudioStatus — get volume, mute, and playback state */
static int handle_get_audio_status(const cJSON *params, cJSON *result) {
    (void)params;
    cJSON_AddBoolToObject(result,   "available", audio_is_available());
    cJSON_AddNumberToObject(result, "volume",    audio_get_volume());
    cJSON_AddBoolToObject(result,   "muted",     audio_get_muted());
    cJSON_AddBoolToObject(result,   "playing",   audio_is_playing());
    return 0;
}
#endif /* CONFIG_BRAMBLE_BOARD_TDECK_PLUS */

/* ── Traffic debug RPC methods ─────────────────────────────────────── */

/* bramble.setTrafficDebug — params: {"enabled":bool, "include_tx":bool, "include_rx":bool, "sample_rate":0-100} */
static int handle_set_traffic_debug(const cJSON *params, cJSON *result) {
    if (!params) return RPC_ERR_INVALID_PARAMS;
    
    /* Default values */
    bool enabled = false;
    bool include_tx = true;
    bool include_rx = true;
    uint8_t sample_rate = 100;
    
    cJSON *en = cJSON_GetObjectItem(params, "enabled");
    if (en && cJSON_IsBool(en)) enabled = cJSON_IsTrue(en);
    
    cJSON *tx = cJSON_GetObjectItem(params, "include_tx");
    if (tx && cJSON_IsBool(tx)) include_tx = cJSON_IsTrue(tx);
    
    cJSON *rx = cJSON_GetObjectItem(params, "include_rx");
    if (rx && cJSON_IsBool(rx)) include_rx = cJSON_IsTrue(rx);
    
    cJSON *sr = cJSON_GetObjectItem(params, "sample_rate");
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

/* bramble.getTrafficDebug — returns current config + buffer state */
static int handle_get_traffic_debug(const cJSON *params, cJSON *result) {
    (void)params;
    
    bool enabled, include_tx, include_rx;
    uint8_t sample_rate;
    mesh_traffic_debug_get_config(&enabled, &include_tx, &include_rx, &sample_rate);
    
    traffic_debug_t *td = mesh_get_traffic_debug();
    
    cJSON_AddBoolToObject(result, "enabled", enabled);
    cJSON_AddBoolToObject(result, "include_tx", include_tx);
    cJSON_AddBoolToObject(result, "include_rx", include_rx);
    cJSON_AddNumberToObject(result, "sample_rate", sample_rate);
    cJSON_AddNumberToObject(result, "buffer_capacity", 512);
    cJSON_AddNumberToObject(result, "buffer_count", traffic_debug_get_count(td));
    cJSON_AddNumberToObject(result, "dropped_count", traffic_debug_get_dropped(td));
    
    return 0;
}

/* bramble.getTrafficEvents — params: {"since_seq":uint32, "limit":uint16} */
static int handle_get_traffic_events(const cJSON *params, cJSON *result) {
    traffic_debug_t *td = mesh_get_traffic_debug();
    
    uint32_t since_seq = 0;
    uint16_t limit = 100;  /* default limit */
    
    if (params) {
        cJSON *seq = cJSON_GetObjectItem(params, "since_seq");
        if (seq && cJSON_IsNumber(seq)) since_seq = (uint32_t)seq->valuedouble;
        
        cJSON *lim = cJSON_GetObjectItem(params, "limit");
        if (lim && cJSON_IsNumber(lim)) {
            int val = lim->valueint;
            limit = (val <= 0) ? 100 : (val > 512 ? 512 : (uint16_t)val);
        }
    }
    
    cJSON *events = cJSON_AddArrayToObject(result, "events");
    
    uint16_t count = traffic_debug_get_count(td);
    uint16_t returned = 0;
    
    for (uint16_t i = 0; i < count && returned < limit; i++) {
        const traffic_event_t *evt = traffic_debug_get_event(td, i);
        if (!evt) break;
        
        /* Filter by seq if requested */
        if (since_seq > 0 && evt->seq <= since_seq) continue;
        
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "seq", evt->seq);
        cJSON_AddNumberToObject(obj, "timestamp_ms", evt->timestamp_ms);
        cJSON_AddNumberToObject(obj, "pkt_type", evt->pkt_type);
        
        /* Category as string */
        static const char *cat_names[] = {
            "beacon", "timesync", "routing", "ack", "chat", "maintenance", "other"
        };
        if (evt->category < 7) {
            cJSON_AddStringToObject(obj, "category", cat_names[evt->category]);
        } else {
            cJSON_AddStringToObject(obj, "category", "unknown");
        }
        
        /* Airtime tier as string */
        static const char *tier_names[] = { "none", "normal", "critical", "broadcast" };
        if (evt->airtime_tier <= 3) {
            cJSON_AddStringToObject(obj, "airtime_tier", tier_names[evt->airtime_tier]);
        } else {
            cJSON_AddStringToObject(obj, "airtime_tier", "unknown");
        }
        
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

/* bramble.setBeaconPolicy — params: {"enabled":bool, "mode":str, "baseIntervalMs":num, ...} */
static int handle_set_beacon_policy(const cJSON *params, cJSON *result) {
    if (!params) return RPC_ERR_INVALID_PARAMS;
    
    beacon_policy_config_t config;
    mesh_get_beacon_policy(&config);  /* Start with current config */
    
    /* Parse parameters */
    cJSON *enabled = cJSON_GetObjectItem(params, "enabled");
    cJSON *mode_str = cJSON_GetObjectItem(params, "mode");
    cJSON *base_ms = cJSON_GetObjectItem(params, "baseIntervalMs");
    cJSON *min_ms = cJSON_GetObjectItem(params, "minIntervalMs");
    cJSON *max_ms = cJSON_GetObjectItem(params, "maxIntervalMs");
    cJSON *dense_th = cJSON_GetObjectItem(params, "denseThreshold");
    cJSON *churn_th = cJSON_GetObjectItem(params, "churnThreshold");
    cJSON *churn_win = cJSON_GetObjectItem(params, "churnWindowMs");
    
    if (enabled && cJSON_IsBool(enabled)) {
        config.enabled = cJSON_IsTrue(enabled);
    }
    
    if (mode_str && cJSON_IsString(mode_str)) {
        const char *mode = cJSON_GetStringValue(mode_str);
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

/* bramble.getBeaconPolicy — returns config and status */
static int handle_get_beacon_policy(const cJSON *params, cJSON *result) {
    (void)params;
    
    beacon_policy_config_t config;
    beacon_policy_status_t status;
    
    mesh_get_beacon_policy(&config);
    mesh_get_beacon_status(&status);
    
    /* Config */
    cJSON *cfg = cJSON_AddObjectToObject(result, "config");
    cJSON_AddBoolToObject(cfg, "enabled", config.enabled);
    cJSON_AddStringToObject(cfg, "mode", 
        config.mode == BEACON_MODE_FIXED ? "fixed" : "adaptive");
    cJSON_AddNumberToObject(cfg, "baseIntervalMs", config.base_interval_ms);
    cJSON_AddNumberToObject(cfg, "minIntervalMs", config.min_interval_ms);
    cJSON_AddNumberToObject(cfg, "maxIntervalMs", config.max_interval_ms);
    cJSON_AddNumberToObject(cfg, "denseThreshold", config.dense_threshold);
    cJSON_AddNumberToObject(cfg, "churnThreshold", config.churn_threshold);
    cJSON_AddNumberToObject(cfg, "churnWindowMs", config.churn_window_ms);
    
    /* Status */
    cJSON *st = cJSON_AddObjectToObject(result, "status");
    cJSON_AddStringToObject(st, "activeMode",
        status.active_mode == BEACON_MODE_FIXED ? "fixed" : "adaptive");
    cJSON_AddNumberToObject(st, "currentIntervalMs", status.current_interval_ms);
    cJSON_AddNumberToObject(st, "neighborCount", status.neighbor_count);
    cJSON_AddNumberToObject(st, "churnEvents", status.churn_events);
    cJSON_AddNumberToObject(st, "lastTransitionMs", status.last_transition_ms);
    cJSON_AddBoolToObject(st, "inBackoff", status.in_backoff);
    
    return 0;
}

void rpc_methods_init(bramble_identity_t *identity) {
    s_identity = identity;

    /* Query methods */
    rpc_register("bramble.getStatus",    handle_get_status);
    rpc_register("bramble.getIdentity",  handle_get_identity);
    rpc_register("bramble.getVersion",   handle_get_version);
    rpc_register("bramble.getNeighbors", handle_get_neighbors);
    rpc_register("bramble.getRoutes",    handle_get_routes);
    rpc_register("bramble.getAirtime",   handle_get_airtime);
    rpc_register("bramble.ping",         handle_ping);
    rpc_register("bramble.getConfig",    handle_get_config);
    rpc_register("bramble.getMessages",  handle_get_messages);
    rpc_register("bramble.getPeerLocations", handle_get_peer_locations);

    /* Action methods */
    rpc_register("bramble.sendMessage",          handle_send_message);
    rpc_register("bramble.sendBroadcast",        handle_send_broadcast);
    rpc_register("bramble.reboot",               handle_reboot);
    rpc_register("bramble.sendProbe",            handle_send_probe);
    rpc_register("bramble.setRadio",             handle_set_radio);
    rpc_register("bramble.setNodeName",          handle_set_node_name);
    rpc_register("bramble.addChannel",           handle_add_channel);
    rpc_register("bramble.removeChannel",        handle_remove_channel);
    rpc_register("bramble.setDefaultChannel",    handle_set_default_channel);
    rpc_register("bramble.setMailbox",           handle_set_mailbox);
    rpc_register("bramble.setLocationConfig",    handle_set_location_config);
    rpc_register("bramble.setLocationContact",   handle_set_location_contact);
    rpc_register("bramble.removeLocationContact",handle_remove_location_contact);
    rpc_register("bramble.shareLocationOnce",    handle_share_location_once);
    rpc_register("bramble.otaUpdate",            handle_ota_update);
    rpc_register("bramble.getBattery",           handle_get_battery);
    rpc_register("bramble.setBacklight",         handle_set_backlight);
    rpc_register("bramble.sleep",               handle_sleep);

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
    rpc_register("bramble.getGpsPosition",       handle_get_gps_position);
    rpc_register("bramble.getStorageInfo",       handle_get_storage_info);
    rpc_register("bramble.playTone",             handle_play_tone);
    rpc_register("bramble.setVolume",            handle_set_volume);
    rpc_register("bramble.setMuted",             handle_set_muted);
    rpc_register("bramble.getAudioStatus",       handle_get_audio_status);
#endif

    /* Traffic debug methods */
    rpc_register("bramble.setTrafficDebug",      handle_set_traffic_debug);
    rpc_register("bramble.getTrafficDebug",      handle_get_traffic_debug);
    rpc_register("bramble.getTrafficEvents",     handle_get_traffic_events);

    /* Beacon policy methods */
    rpc_register("bramble.setBeaconPolicy",      handle_set_beacon_policy);
    rpc_register("bramble.getBeaconPolicy",      handle_get_beacon_policy);

    ESP_LOGI(TAG, "RPC methods registered");
}
