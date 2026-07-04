#include "security.h"
#include <string.h>
#include <stdlib.h>

void rreq_rate_init(rreq_rate_limiter_t* rl) { memset(rl, 0, sizeof(*rl)); }

bool rreq_rate_allow(rreq_rate_limiter_t* rl, uint32_t neighbor, uint32_t dest, uint32_t now_ms) {
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

void rreq_fwd_init(rreq_fwd_limiter_t* rl, uint32_t now_ms) {
    rl->tokens = RREQ_FWD_BURST;
    rl->last_refill_ms = now_ms;
}

bool rreq_fwd_allow(rreq_fwd_limiter_t* rl, uint32_t now_ms) {
    // Integer-only refill: count only whole elapsed refill windows, then advance
    // last_refill_ms by exactly that many windows (not to now_ms). This keeps the
    // leftover sub-window elapsed time for the next call instead of discarding it,
    // so a steady sub-window-interval flood still accrues tokens at the correct
    // long-run rate. whole_windows * RREQ_FWD_REFILL_MS <= elapsed by construction,
    // so the last_refill_ms update below cannot overflow past now_ms.
    uint32_t elapsed = now_ms - rl->last_refill_ms;
    uint32_t whole_windows = elapsed / RREQ_FWD_REFILL_MS;
    if (whole_windows > 0) {
        rl->tokens += whole_windows;
        if (rl->tokens > RREQ_FWD_BURST) {
            rl->tokens = RREQ_FWD_BURST;
        }
        rl->last_refill_ms += whole_windows * RREQ_FWD_REFILL_MS;
    }

    if (rl->tokens >= 1) {
        rl->tokens -= 1;
        return true;
    }
    return false;
}

bool sybil_check_rssi_cluster(const int8_t* rssi_values, int count) {
    if (count < SYBIL_MIN_SUSPECTS)
        return false;

    // For each RSSI value, count how many others are within threshold
    for (int i = 0; i < count; i++) {
        int cluster = 0;
        for (int j = 0; j < count; j++) {
            int diff = rssi_values[i] - rssi_values[j];
            if (diff < 0)
                diff = -diff;
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
