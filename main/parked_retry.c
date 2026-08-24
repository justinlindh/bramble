#include "parked_retry.h"

#include <stddef.h>

/* 0 is the disarmed marker, so a deadline that lands exactly on 0 (once per
 * wrap of the 32-bit millisecond uptime) is nudged a millisecond rather than
 * silently disarming the peer. */
static uint32_t deadline_at(uint32_t t) { return t != 0 ? t : 1u; }

/* Signed difference throughout: now_ms is a wrapping 32-bit uptime
 * (mesh_task.c now_ms), and a plain >= stops firing for 49 days after a wrap. */
static bool elapsed(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

bool parked_retry_sweep_due(parked_sweep_t* s, uint32_t now_ms) {
    if (!elapsed(now_ms, s->next_sweep_ms))
        return false;
    /* Rescheduled whether or not the caller finds anything to do, so a node
     * with nothing parked touches the message store once per interval rather
     * than once per maintenance tick. */
    s->next_sweep_ms = deadline_at(now_ms + PARKED_RETRY_SWEEP_MS);
    return true;
}

void parked_retry_swept(parked_sweep_t* s, uint32_t peer_addr, uint32_t now_ms) {
    s->last_peer = peer_addr;
    s->hold_until_ms = deadline_at(now_ms + PARKED_RETRY_COOLDOWN_MS);
}

void parked_retry_sweep_skipped(parked_sweep_t* s, uint32_t peer_addr) {
    s->last_peer = peer_addr;
    /* The hold belongs to whichever peer was ATTEMPTED, and last_peer has just
     * stopped being that peer. Leaving it set would suppress the beacon
     * trigger for a peer the sweep never touched, which is the one thing this
     * function exists not to do. Today a previous hold has always expired by
     * now, since a sweep runs no more often than a cooldown, so this is a trap
     * rather than a live bug: clearing it means the trap cannot spring if
     * those two intervals are ever tuned apart. */
    s->hold_until_ms = 0;
}

bool parked_retry_sweep_defers_to_beacon(neighbor_table_t* table, uint32_t peer_addr) {
    const neighbor_entry_t* e = neighbor_lookup(table, peer_addr);
    return e != NULL && e->parked_retry_after_ms != 0;
}

bool parked_retry_arm(neighbor_table_t* table, uint32_t peer_addr, uint32_t now_ms) {
    neighbor_entry_t* e = neighbor_lookup(table, peer_addr);
    if (!e)
        return false;
    /* Due now, not one cooldown out: the user has just asked for this message
     * to go when it can, so the first attempt rides the peer's next beacon.
     * The cooldown governs REPEATS (parked_retry_flushed), which is where the
     * airtime risk lives. Each fresh park is a fresh deliberate act on a
     * distinct failed message (a QUEUED row is not parkable again), so this
     * cutting a cooldown short is bounded by the user, not by the mesh. */
    e->parked_retry_after_ms = deadline_at(now_ms);
    return true;
}

bool parked_retry_beacon_decide_flush(neighbor_table_t* table, const parked_sweep_t* sweep,
                                      uint32_t peer_addr, bool is_new_peer, uint32_t now_ms) {
    /* Checked before the rejoin edge, not after, because the rejoin edge is
     * exactly how this collides: the sweep tries a peer that is not in the
     * table, the peer then turns up and is ADMITTED, and an unconditional
     * flush there would queue the same rows a second time while the first
     * attempt is still sitting in the send queue. One slot is enough state for
     * this, because a sweep attempts at most one peer per interval.
     *
     * DEFERRED, not dropped. The held edge is often the only one this peer
     * will ever get: it was not in the table when it was parked for, so
     * nothing armed it, and now that it IS in the table the sweep will pass
     * over it as a neighbor forever after. Arming the entry to the moment the
     * hold ends hands the edge to the armed branch below instead of losing
     * it. Never pulls an existing deadline earlier. */
    if (peer_addr != 0 && sweep != NULL && sweep->last_peer == peer_addr &&
        !elapsed(now_ms, sweep->hold_until_ms)) {
        neighbor_entry_t* held = neighbor_lookup(table, peer_addr);
        if (held && (held->parked_retry_after_ms == 0 ||
                     elapsed(sweep->hold_until_ms, held->parked_retry_after_ms))) {
            held->parked_retry_after_ms = sweep->hold_until_ms;
        }
        return false;
    }

    /* The rejoin edge: a beacon that ADMITTED this address is the
     * "they came back" event, and it flushes whether or not anything armed the
     * entry, because an address that was not in the table could not be armed. */
    if (is_new_peer)
        return true;
    const neighbor_entry_t* e = neighbor_lookup(table, peer_addr);
    if (!e || e->parked_retry_after_ms == 0)
        return false;
    return elapsed(now_ms, e->parked_retry_after_ms);
}

void parked_retry_flushed(neighbor_table_t* table, uint32_t peer_addr, int found, uint32_t now_ms) {
    neighbor_entry_t* e = neighbor_lookup(table, peer_addr);
    if (!e)
        return;
    /* found counts rows the flush picked up, not rows it delivered. A row that
     * did go out is no longer parked, so the next attempt finds nothing and
     * disarms then; a row that did not is still parked and earns another
     * attempt a cooldown later. */
    e->parked_retry_after_ms = (found > 0) ? deadline_at(now_ms + PARKED_RETRY_COOLDOWN_MS) : 0;
}
