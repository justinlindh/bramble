#ifndef BRAMBLE_TIMESYNC_H
#define BRAMBLE_TIMESYNC_H
#include <stdint.h>
#include <stdbool.h>

#define MAX_TIME_SHIFT_MS 2000       /* Tighter clamp: ±2s per sync (was ±5s) */
#define TIME_SYNC_INTERVAL_MS 300000
#define MAX_STRATUM 7
#define CORROBORATION_REQUIRED 3    /* Require 3 corroborating sources for stratum-0 (was 2) */

typedef struct {
    int64_t  offset_ms;
    uint8_t  stratum;
    uint32_t last_sync_ms;
    uint32_t last_emit_ms;
    int64_t  pending_offsets[8];
    uint8_t  pending_strata[8];
    int      pending_count;
    bool     synchronized;
} timesync_state_t;

void timesync_init(timesync_state_t *ts);
int  timesync_handle_sync(timesync_state_t *ts, int64_t remote_time_ms, uint8_t remote_stratum, uint32_t local_now_ms);
int64_t timesync_get_network_time(const timesync_state_t *ts, uint32_t local_now_ms);
bool timesync_should_emit(const timesync_state_t *ts, uint32_t local_now_ms);
void timesync_mark_emitted(timesync_state_t *ts, uint32_t local_now_ms);
uint8_t timesync_get_stratum(const timesync_state_t *ts);
#endif
