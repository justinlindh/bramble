#include "parked_retry.h"

/* 0 is the disarmed marker, so a deadline that lands exactly on 0 (once per
 * wrap of the 32-bit millisecond uptime) is nudged a millisecond rather than
 * silently disarming the peer. */
static uint32_t deadline_at(uint32_t t) { return t != 0 ? t : 1u; }

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

bool parked_retry_beacon_should_flush(neighbor_table_t* table, uint32_t peer_addr, bool is_new_peer,
                                      uint32_t now_ms) {
    /* The rejoin edge, unchanged: a beacon that ADMITTED this address is the
     * "they came back" event, and it flushes whether or not anything armed the
     * entry, because an address that was not in the table could not be armed. */
    if (is_new_peer)
        return true;
    const neighbor_entry_t* e = neighbor_lookup(table, peer_addr);
    if (!e || e->parked_retry_after_ms == 0)
        return false;
    /* Signed difference: now_ms wraps every 49 days (mesh_task.c now_ms), and
     * a plain >= would stop firing for the whole span after a wrap. */
    return (int32_t)(now_ms - e->parked_retry_after_ms) >= 0;
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
