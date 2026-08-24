#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "addr_hex.h"
#include "cJSON.h"

/**
 * Append the "relayPath" array to `parent`, one object per relay hop. This is
 * the single definition of the on-wire relay-path shape shared by the onAck,
 * onBroadcastDelivery, and getDeliveryEvents payloads.
 *
 * hops[0..count) are the relay addresses; count must already be bounded by the
 * caller. include_rssi picks the hop shape: false emits "addr" alone, matching
 * the sampled broadcast telemetry that reports no per-hop rssi; true also emits
 * "rssi", carrying last_hop_rssi on the final hop and 0 on the rest, since only
 * the destination's reading is measured. Pass last_hop_rssi = 0 for events that
 * measure no rssi at all.
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
