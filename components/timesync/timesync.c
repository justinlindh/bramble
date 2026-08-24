#include "timesync.h"
#include <string.h>

void timesync_init(timesync_state_t* ts) {
    memset(ts, 0, sizeof(*ts));
    ts->stratum = MAX_STRATUM;
    ts->synchronized = false;
}

/* Remove entries older than PENDING_MAX_AGE_MS */
static void purge_stale(timesync_state_t* ts, uint32_t local_now_ms) {
    int write = 0;
    for (int i = 0; i < ts->pending_count; i++) {
        uint32_t age = local_now_ms - ts->pending[i].timestamp_ms;
        if (age <= PENDING_MAX_AGE_MS) {
            if (write != i)
                ts->pending[write] = ts->pending[i];
            write++;
        }
    }
    ts->pending_count = write;
}

/* Count distinct source addresses among ESTABLISHED entries in the pending
 * pool. Only established sources count toward the pre-commit corroboration
 * quorum: a non-established entry can still sit in the pool (and its
 * offset still feeds compute_weighted_offset once a quorum has committed
 * via the post-sync path), it just doesn't help reach the FIRST commit. */
static int count_distinct_established_sources(const timesync_state_t* ts) {
    uint32_t seen[PENDING_POOL_SIZE];
    int n = 0;
    for (int i = 0; i < ts->pending_count; i++) {
        if (!ts->pending[i].established)
            continue;
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (seen[j] == ts->pending[i].source_addr) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            seen[n++] = ts->pending[i].source_addr;
        }
    }
    return n;
}

/* Compute weighted average offset from pending pool.
 * Lower stratum = higher weight (MAX_STRATUM - stratum, min 1). */
static int64_t compute_weighted_offset(const timesync_state_t* ts, uint8_t* best_stratum) {
    int64_t weighted_sum = 0;
    int total_weight = 0;
    uint8_t best = MAX_STRATUM;

    for (int i = 0; i < ts->pending_count; i++) {
        int weight = MAX_STRATUM - ts->pending[i].stratum;
        if (weight < 1)
            weight = 1;
        weighted_sum += ts->pending[i].offset_ms * weight;
        total_weight += weight;
        if (ts->pending[i].stratum < best)
            best = ts->pending[i].stratum;
    }

    if (best_stratum)
        *best_stratum = best;
    if (total_weight <= 0)
        return 0;
    return weighted_sum / total_weight;
}

int timesync_handle_sync(timesync_state_t* ts, int64_t remote_time_ms, uint8_t remote_stratum,
                         uint32_t source_addr, bool source_established, uint32_t local_now_ms) {
    /* Reject if remote stratum is not better than ours (when synchronized) */
    if (ts->synchronized && remote_stratum >= ts->stratum) {
        return -1;
    }

    int64_t proposed_offset = remote_time_ms - (int64_t)local_now_ms;

    if (ts->synchronized) {
        /* Reject large shifts once already synchronized. */
        int64_t shift = proposed_offset - ts->offset_ms;
        if (shift < 0)
            shift = -shift;
        if (shift > MAX_TIME_SHIFT_MS) {
            return -2;
        }
    } else if (ts->pending_count > 0) {
        /* Bootstrap outlier clamp (STAGED, see network_key.h): before the
         * first sync commits, ts->offset_ms is not yet meaningful (it is
         * still its zero-init value), so a lone huge proposal cannot be
         * judged against it the way the synchronized branch above does.
         * remote_time_ms is real epoch-ish network time while
         * local_now_ms is raw uptime since boot, so EVERY legitimate
         * first sync proposes an offset far from zero; clamping against
         * zero would permanently block bootstrap. Instead this clamps a
         * NEW proposal against the CONSENSUS of already-pending, DISTINCT
         * sources: a proposal wildly inconsistent with what other sources
         * have already reported is rejected as an outlier before it can
         * enter the pool and skew the weighted average, rather than
         * silently averaged in. A quorum (CORROBORATION_REQUIRED distinct
         * sources) that genuinely agrees with each other still commits
         * normally; only a proposal that disagrees with an existing
         * quorum-in-progress is rejected here. The very first pending
         * entry (pending_count == 0) has nothing to compare against yet
         * and is always admitted; every later entry is judged against it. */
        uint8_t unused_stratum;
        int64_t consensus = compute_weighted_offset(ts, &unused_stratum);
        int64_t shift = proposed_offset - consensus;
        if (shift < 0)
            shift = -shift;
        if (shift > MAX_TIME_SHIFT_MS) {
            return -2;
        }
    }

    /* Purge stale entries before adding */
    purge_stale(ts, local_now_ms);

    /* Check for duplicate source: update existing entry instead of adding */
    for (int i = 0; i < ts->pending_count; i++) {
        if (ts->pending[i].source_addr == source_addr) {
            ts->pending[i].offset_ms = proposed_offset;
            ts->pending[i].stratum = remote_stratum;
            ts->pending[i].timestamp_ms = local_now_ms;
            ts->pending[i].established = source_established;
            goto check_commit;
        }
    }

    /* Add new entry; if pool full, evict oldest */
    if (ts->pending_count >= PENDING_POOL_SIZE) {
        int oldest = 0;
        for (int i = 1; i < ts->pending_count; i++) {
            if (ts->pending[i].timestamp_ms < ts->pending[oldest].timestamp_ms)
                oldest = i;
        }
        ts->pending[oldest] = (timesync_pending_t){
            .offset_ms = proposed_offset,
            .source_addr = source_addr,
            .timestamp_ms = local_now_ms,
            .stratum = remote_stratum,
            .established = source_established,
        };
    } else {
        ts->pending[ts->pending_count++] = (timesync_pending_t){
            .offset_ms = proposed_offset,
            .source_addr = source_addr,
            .timestamp_ms = local_now_ms,
            .stratum = remote_stratum,
            .established = source_established,
        };
    }

check_commit:;
    /* Require corroboration from distinct ESTABLISHED sources before first
     * sync (anti-Sybil lever). Once synchronized, this gate is
     * skipped entirely (see the outer if), so post-commit re-syncs
     * (self-heal) are unaffected by source tenure. */
    int distinct_established = count_distinct_established_sources(ts);
    if (!ts->synchronized && distinct_established < CORROBORATION_REQUIRED) {
        return 0; /* Accepted but not yet committed */
    }

    /* Commit: compute weighted average and update state */
    uint8_t best_stratum = MAX_STRATUM;
    ts->offset_ms = compute_weighted_offset(ts, &best_stratum);
    ts->stratum = best_stratum + 1;
    ts->last_sync_ms = local_now_ms;
    ts->synchronized = true;

    return 1; /* Accepted and committed */
}

int64_t timesync_get_network_time(const timesync_state_t* ts, uint32_t local_now_ms) {
    return (int64_t)local_now_ms + ts->offset_ms;
}

uint8_t timesync_get_stratum(const timesync_state_t* ts) { return ts->stratum; }

bool timesync_is_confident(const timesync_state_t* ts, uint32_t local_now_ms) {
    return ts->synchronized && (local_now_ms - ts->last_sync_ms) <= CONFIDENCE_MAX_AGE_MS;
}
