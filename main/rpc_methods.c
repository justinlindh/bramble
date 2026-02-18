#include "rpc_methods.h"
#include "rpc_dispatcher.h"
#include "mesh_task.h"
#include "airtime_budget.h"
#include "cJSON.h"
#include "esp_timer.h"

#include <inttypes.h>
#include <string.h>

#define BRAMBLE_VERSION_STR      "0.1.0-dev"
#define BRAMBLE_PROTOCOL_VERSION "0.1.0"
#define BRAMBLE_HARDWARE         "heltec_v3"

static bramble_identity_t *s_identity;

static const char *addr_hex(uint32_t addr, char *buf, size_t len) {
    snprintf(buf, len, "%08" PRIX32, addr);
    return buf;
}

/* bramble.getStatus */
static int handle_get_status(const cJSON *params, cJSON *result) {
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
    char buf[12];
    cJSON_AddStringToObject(result, "address", addr_hex(s_identity->address, buf, sizeof(buf)));
    cJSON_AddStringToObject(result, "pubkey_hash", addr_hex(s_identity->pubkey_hash, buf, sizeof(buf)));
    return 0;
}

/* bramble.getVersion */
static int handle_get_version(const cJSON *params, cJSON *result) {
    cJSON_AddStringToObject(result, "firmware_version", BRAMBLE_VERSION_STR);
    cJSON_AddStringToObject(result, "protocol_version", BRAMBLE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(result, "hardware", BRAMBLE_HARDWARE);
    return 0;
}

/* bramble.getNeighbors */
static int handle_get_neighbors(const cJSON *params, cJSON *result) {
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
    /* TODO: wire up routing table once route_table_t is exposed */
    cJSON_AddArrayToObject(result, "routes");
    return 0;
}

/* bramble.getAirtime */
static int handle_get_airtime(const cJSON *params, cJSON *result) {
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
    char buf[12];
    cJSON_AddBoolToObject(result, "pong", true);
    cJSON_AddStringToObject(result, "address", addr_hex(s_identity->address, buf, sizeof(buf)));
    cJSON_AddStringToObject(result, "protocol_version", BRAMBLE_PROTOCOL_VERSION);
    return 0;
}

void rpc_methods_init(bramble_identity_t *identity) {
    s_identity = identity;
    rpc_register("bramble.getStatus",    handle_get_status);
    rpc_register("bramble.getIdentity",  handle_get_identity);
    rpc_register("bramble.getVersion",   handle_get_version);
    rpc_register("bramble.getNeighbors", handle_get_neighbors);
    rpc_register("bramble.getRoutes",    handle_get_routes);
    rpc_register("bramble.getAirtime",   handle_get_airtime);
    rpc_register("bramble.ping",         handle_ping);
}
