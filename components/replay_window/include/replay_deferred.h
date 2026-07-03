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
#endif
