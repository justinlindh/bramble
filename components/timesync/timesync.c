#include "timesync.h"
#include <string.h>
#include <stdlib.h>

void timesync_init(timesync_state_t *ts) {
    memset(ts, 0, sizeof(*ts));
    ts->stratum = MAX_STRATUM;
    ts->synchronized = false;
}

int timesync_handle_sync(timesync_state_t *ts, int64_t remote_time_ms,
                         uint8_t remote_stratum, uint32_t local_now_ms) {
    // Reject if remote stratum is not better (unless we're unsynchronized)
    if (ts->synchronized && remote_stratum >= ts->stratum) {
        return -1;
    }

    // Calculate proposed offset
    int64_t proposed_offset = remote_time_ms - (int64_t)local_now_ms;

    // Reject shifts that are too large (only if already synchronized)
    if (ts->synchronized) {
        int64_t shift = proposed_offset - ts->offset_ms;
        if (shift < 0) shift = -shift;
        if (shift > MAX_TIME_SHIFT_MS) {
            return -2;
        }
    }

    // Store in pending for weighted average
    if (ts->pending_count < 8) {
        ts->pending_offsets[ts->pending_count] = proposed_offset;
        ts->pending_strata[ts->pending_count] = remote_stratum;
        ts->pending_count++;
    }

    // Compute weighted average of pending offsets (lower stratum = higher weight)
    int64_t weighted_sum = 0;
    int total_weight = 0;
    for (int i = 0; i < ts->pending_count; i++) {
        int weight = MAX_STRATUM - ts->pending_strata[i];
        if (weight < 1) weight = 1;
        weighted_sum += ts->pending_offsets[i] * weight;
        total_weight += weight;
    }
    ts->offset_ms = weighted_sum / total_weight;
    ts->stratum = remote_stratum + 1;
    ts->last_sync_ms = local_now_ms;
    ts->synchronized = true;

    return 0;
}

int64_t timesync_get_network_time(const timesync_state_t *ts, uint32_t local_now_ms) {
    return (int64_t)local_now_ms + ts->offset_ms;
}

bool timesync_should_emit(const timesync_state_t *ts, uint32_t local_now_ms) {
    if (!ts->synchronized || ts->stratum > 2) {
        return false;
    }
    uint32_t elapsed = local_now_ms - ts->last_emit_ms;
    return elapsed >= TIME_SYNC_INTERVAL_MS;
}

void timesync_mark_emitted(timesync_state_t *ts, uint32_t local_now_ms) {
    ts->last_emit_ms = local_now_ms;
}

uint8_t timesync_get_stratum(const timesync_state_t *ts) {
    return ts->stratum;
}
