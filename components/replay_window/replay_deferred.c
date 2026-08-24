#include "replay_deferred.h"
#include <string.h>

void replay_deferred_init(replay_deferred_t* d) { memset(d, 0, sizeof(*d)); }

/*
 * Pick a slot for a new (src, counter) record.
 *
 * A record only stops being needed once it is older than
 * DEFERRED_TTL_S, because past that age replay_deferred_accept rejects the
 * corresponding message on its sent_at check anyway. So an entry younger
 * than the TTL must never be evicted: doing so is the whole attack, where
 * flooding cheap spoofed records pushes a target's record out and reopens
 * the deferred acceptance window for it.
 *
 * Unlike replay_window's time-based idle threshold, this needs no tuning
 * constant: the TTL that already governs acceptance is the same TTL that
 * governs usefulness.
 *
 * Returns NULL when every slot is still within the TTL. Both callers treat
 * NULL as fail-closed, which is coherent: a full table refuses to record
 * AND refuses to accept, so nothing slips through unrecorded.
 */
static replay_dslot_t* dslot_alloc(replay_deferred_t* d, uint32_t now_s) {
    replay_dslot_t* oldest = &d->slots[0];
    for (int i = 0; i < REPLAY_DEFERRED_MAX; i++) {
        replay_dslot_t* s = &d->slots[i];
        if (!s->used)
            return s;
        if ((uint32_t)(now_s - s->seen_s) > (uint32_t)(now_s - oldest->seen_s))
            oldest = s;
    }
    if ((uint32_t)(now_s - oldest->seen_s) <= DEFERRED_TTL_S) {
        d->evict_denied++;
        return NULL;
    }
    d->evictions++;
    return oldest;
}

int replay_deferred_accept(replay_deferred_t* d, uint32_t src, uint64_t counter, uint32_t sent_at_s,
                           uint32_t now_s, int timesync_ok) {
    if (!timesync_ok)
        return REPLAY_BELOW_WINDOW; /* fail closed (NEW-SEC-4) */
    if (sent_at_s > now_s + DEFERRED_SKEW_S)
        return REPLAY_BELOW_WINDOW;
    if (now_s > sent_at_s && (now_s - sent_at_s) > DEFERRED_TTL_S)
        return REPLAY_BELOW_WINDOW;

    for (int i = 0; i < REPLAY_DEFERRED_MAX; i++) {
        replay_dslot_t* s = &d->slots[i];
        if (s->used && s->src == src && s->counter == counter)
            return REPLAY_REJECT_DUP;
    }

    replay_dslot_t* s = dslot_alloc(d, now_s);
    if (!s)
        return REPLAY_BELOW_WINDOW; /* fail closed: a full table refuses to accept */
    s->used = 1;
    s->src = src;
    s->counter = counter;
    s->seen_s = now_s;
    return REPLAY_ACCEPT;
}

void replay_deferred_mark_seen(replay_deferred_t* d, uint32_t src, uint64_t counter,
                               uint32_t now_s) {
    for (int i = 0; i < REPLAY_DEFERRED_MAX; i++) {
        replay_dslot_t* s = &d->slots[i];
        if (s->used && s->src == src && s->counter == counter) {
            s->seen_s = now_s; /* refresh recency, already recorded */
            return;
        }
    }

    replay_dslot_t* s = dslot_alloc(d, now_s);
    if (!s)
        return; /* table saturated with in-TTL records; see dslot_alloc */
    s->used = 1;
    s->src = src;
    s->counter = counter;
    s->seen_s = now_s;
}
