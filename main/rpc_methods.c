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

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define BRAMBLE_VERSION_STR      "0.1.0-dev"
#define BRAMBLE_PROTOCOL_VERSION "0.1.0"
#define BRAMBLE_HARDWARE         "heltec_v3"

#define NVS_NAMESPACE            "bramble"
#define NVS_KEY_NODE_NAME        "node_name"

static const char *TAG = "rpc_methods";
static bramble_identity_t *s_identity;

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
    cJSON_AddStringToObject(result, "hardware", BRAMBLE_HARDWARE);
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
    cJSON_AddStringToObject(result, "hardware", BRAMBLE_HARDWARE);
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
    cJSON_AddNumberToObject(result, "critical_remaining_ms", airtime_budget_remaining(&st.airtime, 0x02));
    cJSON_AddNumberToObject(result, "normal_remaining_ms", airtime_budget_remaining(&st.airtime, 0x01));
    cJSON_AddNumberToObject(result, "broadcast_remaining_ms", airtime_budget_remaining(&st.airtime, 0x03));
    cJSON_AddNumberToObject(result, "critical_max_ms", AIRTIME_BUDGET_CRITICAL_MS);
    cJSON_AddNumberToObject(result, "normal_max_ms", AIRTIME_BUDGET_NORMAL_MS);
    cJSON_AddNumberToObject(result, "broadcast_max_ms", AIRTIME_BUDGET_BROADCAST_MS);
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

/* bramble.sendMessage — params: {"dest":"HEXADDR", "text":"..."} */
static int handle_send_message(const cJSON *params, cJSON *result) {
    const char *dest_str = cJSON_GetStringValue(cJSON_GetObjectItem(params, "dest"));
    const char *text     = cJSON_GetStringValue(cJSON_GetObjectItem(params, "text"));
    if (!dest_str || !text) {
        return RPC_ERR_INVALID_PARAMS;
    }

    uint32_t dest = (uint32_t)strtoul(dest_str, NULL, 16);
    uint32_t pkt_id = mesh_send_message(dest, (const uint8_t *)text, strlen(text));
    if (pkt_id == 0) {
        ESP_LOGW(TAG, "mesh_send_message to %08" PRIX32 " failed", dest);
        cJSON_AddStringToObject(result, "error", "send failed");
        return RPC_ERR_RADIO;
    }

    char pkt_id_str[12];
    snprintf(pkt_id_str, sizeof(pkt_id_str), "%08" PRIX32, pkt_id);
    cJSON_AddStringToObject(result, "packetId", pkt_id_str);
    cJSON_AddStringToObject(result, "status", "sent");
    return 0;
}

/* bramble.sendBroadcast — params: {"text":"..."} */
static int handle_send_broadcast(const cJSON *params, cJSON *result) {
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(params, "text"));
    if (!text) {
        return RPC_ERR_INVALID_PARAMS;
    }

    int rc = mesh_send_broadcast((const uint8_t *)text, strlen(text));
    if (rc == -2) {
        cJSON_AddStringToObject(result, "error", "rate limited");
        return RPC_ERR_RATE_LIMIT;
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "mesh_send_broadcast failed: %d", rc);
        cJSON_AddStringToObject(result, "error", "send failed");
        return RPC_ERR_RADIO;
    }

    cJSON_AddStringToObject(result, "message_id", "TODO");
    cJSON_AddStringToObject(result, "status", "sent");
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
    char buf[12];
    snprintf(buf, sizeof(buf), "%08" PRIX32, probe_id);
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "probe_id", buf);
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

/* bramble.addChannel — stub: params {"name":"...", "psk":"hexkey"} */
static int handle_add_channel(const cJSON *params, cJSON *result) {
    if (!params) return RPC_ERR_INVALID_PARAMS;
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(params, "name"));
    const char *psk = cJSON_GetStringValue(cJSON_GetObjectItem(params, "psk"));
    if (!name || strlen(name) == 0 || strlen(name) > 16) {
        return RPC_ERR_INVALID_PARAMS;
    }

    int idx = mesh_add_channel(name,
                               psk ? (const uint8_t *)psk : NULL,
                               psk ? strlen(psk) : 0);
    if (idx < 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "channel limit reached");
        return 0;
    }

    /* Persist channel to NVS */
    nvs_handle_t nvs;
    if (nvs_open("bramble_ch", NVS_READWRITE, &nvs) == ESP_OK) {
        char key_name[20], key_psk[20];
        snprintf(key_name, sizeof(key_name), "ch%d_name", idx);
        snprintf(key_psk, sizeof(key_psk), "ch%d_psk", idx);
        nvs_set_str(nvs, key_name, name);
        if (psk) nvs_set_str(nvs, key_psk, psk);
        nvs_set_u8(nvs, "ch_count", (uint8_t)mesh_get_channel_count());
        nvs_commit(nvs);
        nvs_close(nvs);
    }

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

    nvs_handle_t nvs;
    if (nvs_open("bramble_ch", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "ch_count", (uint8_t)mesh_get_channel_count());
        nvs_commit(nvs);
        nvs_close(nvs);
    }

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

    /* TODO: add received peer locations once location packets are implemented */
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

    /* Channel list — read from mesh task */
    int ch_count = mesh_get_channel_count();
    cJSON *channels = cJSON_CreateArray();

    /* Read channel names from NVS */
    nvs_handle_t ch_nvs;
    bool ch_nvs_open = (nvs_open("bramble_ch", NVS_READONLY, &ch_nvs) == ESP_OK);

    for (int i = 0; i < ch_count; i++) {
        cJSON *ch = cJSON_CreateObject();
        char ch_name[20] = "";
        if (i == 0) {
            strcpy(ch_name, "public");
        } else if (ch_nvs_open) {
            char key[20];
            snprintf(key, sizeof(key), "ch%d_name", i);
            size_t len = sizeof(ch_name);
            if (nvs_get_str(ch_nvs, key, ch_name, &len) != ESP_OK)
                snprintf(ch_name, sizeof(ch_name), "channel_%d", i);
        }
        cJSON_AddStringToObject(ch, "name", ch_name);
        cJSON_AddNumberToObject(ch, "id", i);
        cJSON_AddBoolToObject(ch, "is_default", i == 0);
        cJSON_AddItemToArray(channels, ch);
    }
    if (ch_nvs_open) nvs_close(ch_nvs);
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
    rpc_register("bramble.sleep",               handle_sleep);

    ESP_LOGI(TAG, "RPC methods registered (query: 13, action: 16)");
}
