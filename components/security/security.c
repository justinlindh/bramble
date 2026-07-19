#include "security.h"
#include <string.h>
#include <stdlib.h>

void rreq_rate_init(rreq_rate_limiter_t* rl) { memset(rl, 0, sizeof(*rl)); }

bool rreq_rate_allow(rreq_rate_limiter_t* rl, uint32_t neighbor, uint32_t dest, uint32_t now_ms) {
    int stale = -1;
    // Look for an existing entry, and along the way remember the first slot
    // whose cooldown has fully elapsed. Such a slot would itself be allowed
    // right now, so it can be reused for a new pair without weakening rate
    // limiting for the pair it held.
    for (int i = 0; i < rl->count; i++) {
        if (rl->entries[i].neighbor_addr == neighbor && rl->entries[i].dest_addr == dest) {
            uint32_t elapsed = now_ms - rl->entries[i].last_rreq_ms;
            if (elapsed < RREQ_RATE_LIMIT_MS) {
                return false;
            }
            rl->entries[i].last_rreq_ms = now_ms;
            return true;
        }
        if (stale < 0 && (now_ms - rl->entries[i].last_rreq_ms) >= RREQ_RATE_LIMIT_MS) {
            stale = i;
        }
    }

    // New pair: use a free slot, else reclaim a stale one. Reclaiming matters
    // because entries are never otherwise removed: without it, an attacker
    // cycling distinct destinations permanently fills all RREQ_RATE_ENTRIES
    // slots, after which every later new pair hits the "table full" path and
    // per-pair rate limiting is silently disabled for good.
    int slot;
    if (rl->count < RREQ_RATE_ENTRIES) {
        slot = rl->count++;
    } else if (stale >= 0) {
        slot = stale;
    } else {
        // Table full of pairs still within their cooldown window. The global
        // rreq_fwd token bucket remains the aggregate backstop, so fail open
        // here rather than evicting a live per-pair limiter.
        return true;
    }
    rl->entries[slot].neighbor_addr = neighbor;
    rl->entries[slot].dest_addr = dest;
    rl->entries[slot].last_rreq_ms = now_ms;
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

/*
 * PROBE ingress buckets (issue #75). Same integer-only refill discipline as
 * rreq_fwd_allow above: whole elapsed windows only, last_refill_ms advanced
 * by exactly that many windows so the sub-window remainder is carried rather
 * than discarded. A caller polling every millisecond must not accrue tokens
 * faster than one polling once, or the ceiling becomes a function of the RX
 * rate, which is precisely what an attacker controls.
 *
 * Uses wrapping unsigned subtraction, so the 32-bit millisecond clock
 * rollover a node reaches at ~49 days of uptime is a no-op rather than a
 * multi-week stall in probe handling.
 */
static bool probe_bucket_take(probe_bucket_t* b, uint32_t burst, uint32_t refill_ms,
                              uint32_t now_ms) {
    uint32_t elapsed = now_ms - b->last_refill_ms;
    uint32_t whole_windows = elapsed / refill_ms;
    if (whole_windows > 0) {
        b->tokens += whole_windows;
        if (b->tokens > burst) {
            b->tokens = burst;
        }
        b->last_refill_ms += whole_windows * refill_ms;
    }

    if (b->tokens >= 1) {
        b->tokens -= 1;
        return true;
    }
    return false;
}

void probe_ingress_init(probe_ingress_limiter_t* rl, uint32_t now_ms) {
    memset(rl, 0, sizeof(*rl));
    rl->reply.tokens = PROBE_REPLY_BURST;
    rl->reply.last_refill_ms = now_ms;
    rl->forward.tokens = PROBE_FWD_BURST;
    rl->forward.last_refill_ms = now_ms;
}

probe_ingress_decision_t probe_ingress_allow(probe_ingress_limiter_t* rl, uint32_t now_ms) {
    probe_ingress_decision_t d = {false, false};

    if (!probe_bucket_take(&rl->reply, PROBE_REPLY_BURST, PROBE_REPLY_REFILL_MS, now_ms)) {
        rl->dropped_reply++;
        return d;
    }

    rl->accepted++;
    d.reply = true;

    /* Forwarding is the amplifying term and carries the tighter budget, so
     * propagation stops before local answers do. */
    if (probe_bucket_take(&rl->forward, PROBE_FWD_BURST, PROBE_FWD_REFILL_MS, now_ms)) {
        d.forward = true;
    } else {
        rl->dropped_forward++;
    }

    return d;
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
