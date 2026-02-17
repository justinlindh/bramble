#include "sim_anomaly.h"
#include "sim_emitter.h"
#include <string.h>

void anomaly_init(route_flap_tracker_t *tracker) {
    memset(tracker, 0, sizeof(*tracker));
}

bool anomaly_check_route_flap(route_flap_tracker_t *tracker, uint32_t dest_addr, uint32_t next_hop, uint64_t now_us, FILE *emit_out, const char *node_id) {
    /* Count recent changes to this destination */
    int flap_count = 0;
    for (int i = 0; i < tracker->count; i++) {
        if (tracker->changes[i].dest_addr == dest_addr) {
            uint64_t age = now_us - tracker->changes[i].timestamp_us;
            if (age < ROUTE_FLAP_WINDOW_US)
                flap_count++;
        }
    }

    /* Add this change */
    int idx;
    if (tracker->count < MAX_ROUTE_FLAP_TRACK) {
        idx = tracker->count++;
    } else {
        /* Evict oldest */
        idx = 0;
        for (int i = 1; i < MAX_ROUTE_FLAP_TRACK; i++) {
            if (tracker->changes[i].timestamp_us < tracker->changes[idx].timestamp_us)
                idx = i;
        }
    }
    tracker->changes[idx].dest_addr = dest_addr;
    tracker->changes[idx].next_hop = next_hop;
    tracker->changes[idx].timestamp_us = now_us;

    /* Detect flap */
    if (flap_count >= ROUTE_FLAP_THRESHOLD) {
        char details[128];
        snprintf(details, sizeof(details), "%d changes in 2s", flap_count);
        emit_anomaly(emit_out, now_us, "route_flap", node_id, dest_addr, details);
        return true;
    }

    return false;
}
