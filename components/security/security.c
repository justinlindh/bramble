#include "security.h"
#include <string.h>
#include <stdlib.h>

void rreq_rate_init(rreq_rate_limiter_t *rl) {
    memset(rl, 0, sizeof(*rl));
}

bool rreq_rate_allow(rreq_rate_limiter_t *rl, uint32_t neighbor, uint32_t dest, uint32_t now_ms) {
    // Look for existing entry
    for (int i = 0; i < rl->count; i++) {
        if (rl->entries[i].neighbor_addr == neighbor && rl->entries[i].dest_addr == dest) {
            uint32_t elapsed = now_ms - rl->entries[i].last_rreq_ms;
            if (elapsed < RREQ_RATE_LIMIT_MS) {
                return false;
            }
            rl->entries[i].last_rreq_ms = now_ms;
            return true;
        }
    }

    // New entry
    if (rl->count < RREQ_RATE_ENTRIES) {
        rl->entries[rl->count].neighbor_addr = neighbor;
        rl->entries[rl->count].dest_addr = dest;
        rl->entries[rl->count].last_rreq_ms = now_ms;
        rl->count++;
    }
    return true;
}

bool sybil_check_rssi_cluster(const int8_t *rssi_values, int count) {
    if (count < SYBIL_MIN_SUSPECTS) return false;

    // For each RSSI value, count how many others are within threshold
    for (int i = 0; i < count; i++) {
        int cluster = 0;
        for (int j = 0; j < count; j++) {
            int diff = rssi_values[i] - rssi_values[j];
            if (diff < 0) diff = -diff;
            if (diff <= SYBIL_RSSI_CLUSTER_THRESHOLD) {
                cluster++;
            }
        }
        if (cluster >= SYBIL_MIN_SUSPECTS) {
            return true;
        }
    }
    return false;
}
