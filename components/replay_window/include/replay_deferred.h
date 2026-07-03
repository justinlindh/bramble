#ifndef BRAMBLE_REPLAY_DEFERRED_H
#define BRAMBLE_REPLAY_DEFERRED_H
#include <stdint.h>
#include "replay_window.h" /* REPLAY_ACCEPT / REPLAY_REJECT_DUP / REPLAY_BELOW_WINDOW */

#define REPLAY_DEFERRED_MAX 128

/* Deferred (tier-2) acceptance window for a message whose nonce counter
 * fell below the tier-1 sliding window (replay_window.h), e.g. a delayed
 * store-and-forward chat message. Only accepted when the authenticated
 * sent_at is within [now - DEFERRED_TTL_S, now + DEFERRED_SKEW_S] AND time
 * is trusted AND (src_addr, counter) has not already been accepted. */
#define DEFERRED_SKEW_S 300  /* allow sent_at up to 5 minutes in the future */
#define DEFERRED_TTL_S 86400 /* reject sent_at older than 24 hours */

typedef struct {
    uint32_t src;
    uint64_t counter;
    uint32_t seen_s;
    uint8_t used;
} replay_dslot_t;

typedef struct {
    replay_dslot_t slots[REPLAY_DEFERRED_MAX];
} replay_deferred_t;

void replay_deferred_init(replay_deferred_t* d);

/*
 * Fail-closed under untrusted time (NEW-SEC-4): returns non-accept whenever
 * timesync_ok is false, regardless of counter/sent_at. sent_at_s and now_s
 * must be on the same clock basis (network time), not raw device uptime,
 * since they come from different nodes.
 */
int replay_deferred_accept(replay_deferred_t* d, uint32_t src, uint64_t counter, uint32_t sent_at_s,
                           uint32_t now_s, int timesync_ok);

/*
 * Fix 3 (red-team panel, post-Task-3.6): records that (src, counter) has
 * already been legitimately delivered via the TIER-1 sliding window
 * (replay_window.h), independent of and with no time/skew validation
 * (the caller already accepted it through a different, non-time-based
 * path). Without this, a counter accepted by tier-1 and later aged out of
 * its 64-entry window is in NEITHER dedup structure: replaying the
 * original captured packet reads BELOW_WINDOW at tier-1, then tier-2
 * (replay_deferred_accept) sees a src/counter pair it has never heard of
 * and re-accepts it as if it were a legitimate delayed delivery. Callers
 * must invoke this on every tier-1 REPLAY_ACCEPT for a CHAT message (the
 * only app type replay_deferred_accept ever defers), not just on
 * below-window arrivals.
 */
void replay_deferred_mark_seen(replay_deferred_t* d, uint32_t src, uint64_t counter,
                               uint32_t now_s);
#endif
