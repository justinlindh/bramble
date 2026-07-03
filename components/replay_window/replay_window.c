#include "include/replay_window.h"
#include <string.h>

void replay_table_init(replay_table_t* t) { memset(t, 0, sizeof(*t)); }

/* Find or LRU-allocate a slot for src_addr. A slot handed to a (possibly
 * new) sender always starts with seen = 0: both the never-used path and the
 * LRU-reused path must reset it, or a reused slot's leftover seen = 1 from
 * its PREVIOUS occupant would skip the fresh-slot branch for the new
 * sender's first packet and misclassify it via the general accept/dup path
 * (the same failure mode BUG A fixes at init, just reachable through
 * eviction instead). */
static replay_slot_t* slot_for(replay_table_t* t, uint32_t src_addr, uint32_t now_ms) {
    replay_slot_t* lru = &t->slots[0];
    for (int i = 0; i < REPLAY_MAX_SENDERS; i++) {
        replay_slot_t* s = &t->slots[i];
        if (s->used && s->src_addr == src_addr) return s;
        if (!s->used) { s->used = 1; s->src_addr = src_addr; s->high_water = 0;
                        s->window = 0; s->seen = 0; s->last_seen_ms = now_ms; return s; }
        if (s->last_seen_ms < lru->last_seen_ms) lru = s;
    }
    lru->src_addr = src_addr; lru->high_water = 0; lru->window = 0;
    lru->seen = 0; lru->last_seen_ms = now_ms; return lru;
}

int replay_check_and_add(replay_table_t* t, uint32_t src_addr, uint64_t counter, uint32_t now_ms) {
    replay_slot_t* s = slot_for(t, src_addr, now_ms);
    s->last_seen_ms = now_ms;

    /* BUG A fix: a fresh slot is "no packet seen yet", not "high_water
     * happens to be 0". The nonce counter (Task 0.4) issues 0 as the very
     * first value on a node's first-ever boot, so high_water == 0 is a
     * legitimate, already-seen counter value, not just an unset sentinel.
     * Conflating the two lets a replay of that real counter-0 packet re-hit
     * the "fresh slot" branch and be accepted again, forever. */
    if (!s->seen) {
        s->seen = 1;
        s->high_water = counter;
        return REPLAY_ACCEPT;
    }

    if (counter > s->high_water) {
        uint64_t shift = counter - s->high_water;
        /* BUG B fix: at shift == 64 the old high_water sits at window bit 63
         * (the last bit the 64-bit window can represent) and must survive
         * the shift, not be wiped to 0. A naive `shift >= 64 -> window = 0`
         * loses that bit, so replaying the old high_water after an exact
         * 64-counter jump would wrongly be accepted. `window << 64` is also
         * undefined behavior in C, so shift must never reach the shift
         * operator at 64 or above. */
        if (shift > 64) {
            s->window = 0;
        } else if (shift == 64) {
            s->window = (1ull << 63); /* only the old high_water still tracked */
        } else {
            s->window = (s->window << shift) | (1ull << (shift - 1));
        }
        s->high_water = counter;
        return REPLAY_ACCEPT;
    }

    uint64_t delta = s->high_water - counter;
    if (delta == 0) return REPLAY_REJECT_DUP;
    if (delta > 64) return REPLAY_BELOW_WINDOW;
    uint64_t mask = 1ull << (delta - 1);
    if (s->window & mask) return REPLAY_REJECT_DUP;
    s->window |= mask;
    return REPLAY_ACCEPT;
}
