#include "util.h"

#include <inttypes.h>
#include <stdio.h>

const char* addr_hex(uint32_t addr, char* buf, size_t len) {
    snprintf(buf, len, "%08" PRIX32, addr);
    return buf;
}

void traffic_event_add_json(cJSON* obj, const traffic_event_t* evt) {
    cJSON_AddNumberToObject(obj, "seq", evt->seq);
    cJSON_AddNumberToObject(obj, "timestamp_ms", evt->timestamp_ms);
    cJSON_AddNumberToObject(obj, "pkt_type", evt->pkt_type);

    /* Category and airtime tier as canonical strings, from the traffic_debug
     * component so the enum and its name cannot drift apart. */
    cJSON_AddStringToObject(obj, "category", traffic_debug_category_name(evt->category));
    cJSON_AddStringToObject(obj, "airtime_tier",
                            traffic_debug_airtime_tier_name(evt->airtime_tier));

    cJSON_AddNumberToObject(obj, "packet_len", evt->packet_len);
    cJSON_AddNumberToObject(obj, "rssi", evt->rssi);
    cJSON_AddBoolToObject(obj, "is_tx", evt->is_tx);

    if (evt->src_addr != 0) {
        char src_buf[12];
        cJSON_AddStringToObject(obj, "src_addr", addr_hex(evt->src_addr, src_buf, sizeof(src_buf)));
    }
}
