#include "include/replay_deferred.h"
#include <string.h>

void replay_deferred_init(replay_deferred_t* d) { memset(d, 0, sizeof(*d)); }

int replay_deferred_accept(replay_deferred_t* d, uint32_t src, uint64_t counter,
                           uint32_t sent_at_s, uint32_t now_s, int timesync_ok) {
    if (!timesync_ok) return REPLAY_BELOW_WINDOW;           /* fail closed (NEW-SEC-4) */
    if (sent_at_s > now_s + DEFERRED_SKEW_S) return REPLAY_BELOW_WINDOW;
    if (now_s > sent_at_s && (now_s - sent_at_s) > DEFERRED_TTL_S) return REPLAY_BELOW_WINDOW;

    replay_dslot_t* lru = &d->slots[0];
    for (int i = 0; i < REPLAY_DEFERRED_MAX; i++) {
        replay_dslot_t* s = &d->slots[i];
        if (s->used && s->src == src && s->counter == counter) return REPLAY_REJECT_DUP;
        if (!s->used) { s->used = 1; s->src = src; s->counter = counter;
                        s->seen_s = now_s; return REPLAY_ACCEPT; }
        if (s->seen_s < lru->seen_s) lru = s;
    }
    lru->src = src; lru->counter = counter; lru->seen_s = now_s; return REPLAY_ACCEPT;
}
