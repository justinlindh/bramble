#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "addr_hex.h"
#include "cJSON.h"

/**
 * Append the shared "relayPath" JSON array to `parent`: one hop object per
 * relay address, under the key "relayPath". This is the single source of the
 * on-wire relay-path shape used by the onAck, onBroadcastDelivery, and
 * getDeliveryEvents payloads, which otherwise built the same array by hand in
 * three places.
 *
 * hops[0..count) are the relay addresses; count is already bounded by the
 * caller. When include_rssi is true each hop carries "rssi": 0 for every hop
 * except the last, which carries last_hop_rssi (an ACK/receipt's measured
 * rssi_at_dest); pass last_hop_rssi = 0 for events that report no measured
 * rssi. When include_rssi is false the hops carry "addr" only, matching the
 * sampled broadcast telemetry that does not report a per-hop rssi.
 */
static inline void relay_path_json_add(cJSON* parent, const uint32_t* hops, uint8_t count,
                                       bool include_rssi, int8_t last_hop_rssi) {
    cJSON* path = cJSON_AddArrayToObject(parent, "relayPath");
    char hop_buf[9];
    for (uint8_t i = 0; i < count; i++) {
        cJSON* hop = cJSON_CreateObject();
        cJSON_AddStringToObject(hop, "addr", addr_hex(hops[i], hop_buf, sizeof(hop_buf)));
        if (include_rssi) {
            cJSON_AddNumberToObject(hop, "rssi", (i == count - 1) ? last_hop_rssi : 0);
        }
        cJSON_AddItemToArray(path, hop);
    }
}
