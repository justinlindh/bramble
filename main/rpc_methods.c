#include "rpc_methods.h"
#include "rpc_dispatcher.h"
#include "mesh_task.h"
#include "msg_store.h"
#include "airtime_budget.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

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
    mesh_shared_state_t st;
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
    mesh_shared_state_t st;
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
        cJSON_AddNumberToObject(obj, "last_seen_ms", n->last_heard);
        cJSON_AddItemToArray(arr, obj);
    }
    return 0;
}

/* bramble.getRoutes */
static int handle_get_routes(const cJSON *params, cJSON *result) {
    (void)params;
    /* TODO: wire up routing table once route_table_t is exposed */
    cJSON_AddArrayToObject(result, "routes");
    return 0;
}

/* bramble.getAirtime */
static int handle_get_airtime(const cJSON *params, cJSON *result) {
    (void)params;
    /* TODO: expose airtime_budget_t from mesh task for real data */
    cJSON_AddNumberToObject(result, "critical_remaining_ms", 0);
    cJSON_AddNumberToObject(result, "normal_remaining_ms", 0);
    cJSON_AddNumberToObject(result, "broadcast_remaining_ms", 0);
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
    int rc = mesh_send_message(dest, (const uint8_t *)text, strlen(text));
    if (rc != 0) {
        ESP_LOGW(TAG, "mesh_send_message to %08" PRIX32 " failed: %d", dest, rc);
        cJSON_AddStringToObject(result, "error", "send failed");
        return RPC_ERR_RADIO;
    }

    /* TODO: generate a proper message ID (sequence counter + address) */
    cJSON_AddStringToObject(result, "message_id", "TODO");
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
    const char *dest_str = cJSON_GetStringValue(cJSON_GetObjectItem(params, "dest"));
    if (!dest_str) {
        return RPC_ERR_INVALID_PARAMS;
    }
    /* TODO: implement route probe / link test */
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "note", "probe not yet implemented");
    return 0;
}

/* bramble.setRadio — stub: params {"sf":9, "bw_hz":125000, "tx_power":17, "freq_mhz":915.0} */
static int handle_set_radio(const cJSON *params, cJSON *result) {
    (void)params;
    /* TODO: validate params, reconfigure radio, persist to NVS */
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "note", "radio reconfiguration not yet implemented");
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

    ESP_LOGI(TAG, "Node name set to: %s", name);
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "name", name);
    return 0;
}

/* bramble.addChannel — stub: params {"name":"...", "psk":"hexkey"} */
static int handle_add_channel(const cJSON *params, cJSON *result) {
    (void)params;
    /* TODO: parse PSK, derive channel key, add to s_channels */
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "note", "addChannel not yet implemented");
    return 0;
}

/* bramble.removeChannel — stub: params {"name":"..."} */
static int handle_remove_channel(const cJSON *params, cJSON *result) {
    (void)params;
    /* TODO: find and remove channel by name */
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "note", "removeChannel not yet implemented");
    return 0;
}

/* bramble.setDefaultChannel — stub: params {"name":"..."} */
static int handle_set_default_channel(const cJSON *params, cJSON *result) {
    (void)params;
    /* TODO: reorder channel list so named channel is index 0 */
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "note", "setDefaultChannel not yet implemented");
    return 0;
}

/* bramble.setMailbox — stub: params {"enabled": bool, "max_messages": int} */
static int handle_set_mailbox(const cJSON *params, cJSON *result) {
    (void)params;
    /* TODO: configure store-and-forward mailbox */
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "note", "setMailbox not yet implemented");
    return 0;
}

/* bramble.setLocationConfig — stub: params {"enabled": bool, "interval_s": int} */
static int handle_set_location_config(const cJSON *params, cJSON *result) {
    (void)params;
    /* TODO: configure GPS / location beacon interval */
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "note", "setLocationConfig not yet implemented");
    return 0;
}

/* bramble.setLocationContact — stub: params {"address":"HEXADDR"} */
static int handle_set_location_contact(const cJSON *params, cJSON *result) {
    (void)params;
    /* TODO: add address to location-sharing whitelist */
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "note", "setLocationContact not yet implemented");
    return 0;
}

/* bramble.removeLocationContact — stub: params {"address":"HEXADDR"} */
static int handle_remove_location_contact(const cJSON *params, cJSON *result) {
    (void)params;
    /* TODO: remove address from location-sharing whitelist */
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "note", "removeLocationContact not yet implemented");
    return 0;
}

/* bramble.shareLocationOnce — stub: params {"address":"HEXADDR"} */
static int handle_share_location_once(const cJSON *params, cJSON *result) {
    (void)params;
    /* TODO: send a single location beacon to the specified address */
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "note", "shareLocationOnce not yet implemented");
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
        cJSON_AddItemToArray(arr, obj);
    }
    return 0;
}

/* bramble.getPeerLocations — stub, returns empty array */
static int handle_get_peer_locations(const cJSON *params, cJSON *result) {
    (void)params;
    /* TODO: return last known location for each peer that shares location */
    cJSON_AddArrayToObject(result, "locations");
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

    /* Radio config — reflect static defaults (until setRadio is implemented) */
    cJSON *radio = cJSON_CreateObject();
    cJSON_AddNumberToObject(radio, "frequency_mhz", 915.0);
    cJSON_AddNumberToObject(radio, "sf", 9);
    cJSON_AddNumberToObject(radio, "bw_hz", 125000);
    cJSON_AddNumberToObject(radio, "tx_power_dbm", 17);
    cJSON_AddStringToObject(radio, "profile", "long_range");
    cJSON_AddItemToObject(result, "radio", radio);

    /* Channel list — public channel always present */
    cJSON *channels = cJSON_CreateArray();
    cJSON *pub = cJSON_CreateObject();
    cJSON_AddStringToObject(pub, "name", "public");
    cJSON_AddNumberToObject(pub, "id", 0);
    cJSON_AddBoolToObject(pub, "is_default", true);
    cJSON_AddItemToArray(channels, pub);
    cJSON_AddItemToObject(result, "channels", channels);

    return 0;
}

/* ── Registration ───────────────────────────────────────────────────── */

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

    ESP_LOGI(TAG, "RPC methods registered (query: 10, action: 14)");
}
