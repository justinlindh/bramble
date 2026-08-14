#include "topology_export.h"

#include "addr_hex.h"

/*
 * Addresses in this document are 8 uppercase hex digits, the format
 * api/openapi.yaml fixes and simulator/gosim/twin_export.go's
 * normalizeTwinAddr parses. addr_hex lives in its own dependency-free header
 * so this file, which compiles into the simulator (simulator/gosim/all.c), can
 * share it without dragging util.h's traffic_debug dependency along.
 */

void topology_export_neighbors(cJSON* arr, const neighbor_table_t* table, uint32_t now_ms) {
    if (!arr || !table) {
        return;
    }
    char buf[12];
    for (int i = 0; i < table->count; i++) {
        const neighbor_entry_t* n = &table->entries[i];
        if (n->addr == 0) {
            continue;
        }
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "address", addr_hex(n->addr, buf, sizeof(buf)));
        cJSON_AddNumberToObject(obj, "rssi", n->rssi);
        cJSON_AddNumberToObject(obj, "snr", n->snr);
        /* Age, not an absolute timestamp: see the header. A last_heard in the
         * future (a clock that moved backwards) reads as zero rather than
         * wrapping into a huge age. */
        cJSON_AddNumberToObject(obj, "last_seen_ms",
                                (now_ms > n->last_heard) ? (now_ms - n->last_heard) : 0);
        if (n->name[0] != '\0') {
            cJSON_AddStringToObject(obj, "name", n->name);
        }
        cJSON_AddItemToArray(arr, obj);
    }
}

void topology_export_routes(cJSON* arr, const routing_table_t* table) {
    if (!arr || !table) {
        return;
    }
    static const char* state_names[] = {"discovering", "unverified", "active", "stale", "broken"};
    char buf[12];
    for (int i = 0; i < table->count; i++) {
        const route_entry_t* r = &table->entries[i];
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
}

void topology_export_document(cJSON* result, const topology_export_identity_t* identity,
                              const topology_export_phy_t* phy, const neighbor_table_t* neighbors,
                              const routing_table_t* routes, uint32_t now_ms) {
    if (!result || !identity || !phy) {
        return;
    }
    char buf[12];

    cJSON_AddNumberToObject(result, "twin_schema", TOPOLOGY_EXPORT_SCHEMA);

    cJSON* node = cJSON_AddObjectToObject(result, "node");
    cJSON_AddStringToObject(node, "address", addr_hex(identity->address, buf, sizeof(buf)));
    if (identity->name && identity->name[0] != '\0') {
        cJSON_AddStringToObject(node, "name", identity->name);
    }
    if (identity->firmware_version) {
        cJSON_AddStringToObject(node, "firmware_version", identity->firmware_version);
    }
    if (identity->protocol_version) {
        cJSON_AddStringToObject(node, "protocol_version", identity->protocol_version);
    }
    if (identity->hardware) {
        cJSON_AddStringToObject(node, "hardware", identity->hardware);
    }
    cJSON_AddNumberToObject(node, "uptime_s", (double)identity->uptime_s);

    cJSON* radio = cJSON_AddObjectToObject(result, "radio");
    cJSON_AddNumberToObject(radio, "frequency_mhz", phy->frequency_mhz);
    cJSON_AddNumberToObject(radio, "sf", phy->sf);
    cJSON_AddNumberToObject(radio, "bw_hz", phy->bw_hz);
    cJSON_AddNumberToObject(radio, "coding_rate", phy->coding_rate);
    cJSON_AddNumberToObject(radio, "tx_power_dbm", phy->tx_power_dbm);
    if (phy->region) {
        cJSON_AddStringToObject(radio, "region", phy->region);
    }
    if (phy->regulatory) {
        cJSON_AddStringToObject(radio, "regulatory", phy->regulatory);
    }
    cJSON_AddNumberToObject(radio, "max_duty_cycle_pct", phy->max_duty_cycle_pct);
    cJSON_AddBoolToObject(radio, "duty_cycle_enforced", phy->duty_cycle_enforced);

    topology_export_neighbors(cJSON_AddArrayToObject(result, "neighbors"), neighbors, now_ms);
    topology_export_routes(cJSON_AddArrayToObject(result, "routes"), routes);
}
