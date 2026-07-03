#ifndef BRAMBLE_TIMESYNC_H
#define BRAMBLE_TIMESYNC_H
#include <stdint.h>
#include <stdbool.h>

/*
 * Bramble mesh time synchronization.
 *
 * Nodes propagate network time via beacon piggyback. Each node maintains
 * a stratum (hop distance from the time root). Lower stratum = more
 * authoritative. Synchronization requires CORROBORATION_REQUIRED distinct
 * sources within PENDING_MAX_AGE_MS before committing an offset.
 *
 * Once synchronized, large jumps (> MAX_TIME_SHIFT_MS) are rejected to
 * prevent single-beacon time manipulation.
 */

#define MAX_TIME_SHIFT_MS 2000    /* Max ±2s shift per sync when already synced */
#define MAX_STRATUM 7             /* Stratum ceiling; nodes at MAX_STRATUM don't emit */
#define CORROBORATION_REQUIRED 3  /* Distinct sources needed before first sync */
#define PENDING_POOL_SIZE 8       /* Max pending offset entries */
#define PENDING_MAX_AGE_MS 180000 /* Pending entries expire after 180s (3 beacon cycles) */

typedef struct {
    int64_t offset_ms;     /* Offset applied to produce network_time */
    uint32_t source_addr;  /* Which node contributed this offset */
    uint32_t timestamp_ms; /* local_now_ms when this entry was recorded */
    uint8_t stratum;       /* Remote stratum of the source */
} timesync_pending_t;

typedef struct {
    int64_t offset_ms;     /* Committed network time offset */
    uint8_t stratum;       /* Our stratum (best source stratum + 1) */
    uint32_t last_sync_ms; /* local_now_ms of last committed sync */
    bool synchronized;     /* Have we committed at least once? */

    timesync_pending_t pending[PENDING_POOL_SIZE];
    int pending_count;
} timesync_state_t;

/* Initialize to unsynchronized, stratum MAX_STRATUM */
void timesync_init(timesync_state_t* ts);

/*
 * Ingest a time observation from a beacon.
 *
 * Returns:
 *   0  = accepted (may or may not commit depending on corroboration)
 *   1  = accepted AND committed (corroboration threshold met)
 *  -1  = rejected: remote stratum not better than ours
 *  -2  = rejected: proposed shift too large (when already synced)
 *  -3  = rejected: duplicate source within pending window
 */
int timesync_handle_sync(timesync_state_t* ts, int64_t remote_time_ms, uint8_t remote_stratum,
                         uint32_t source_addr, uint32_t local_now_ms);

/* Get network time from current offset + local clock */
int64_t timesync_get_network_time(const timesync_state_t* ts, uint32_t local_now_ms);

/* Get our current stratum */
uint8_t timesync_get_stratum(const timesync_state_t* ts);

/*
 * Whether network time is trustworthy enough to gate a security decision on
 * (e.g. deferred replay acceptance, NEW-SEC-4). Placeholder for Task 3.5's
 * stratum/corroboration-aware confidence signal; for now this mirrors
 * `synchronized` (at least one corroborated sync has committed).
 */
bool timesync_is_confident(const timesync_state_t* ts);

#endif
