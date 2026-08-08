#ifndef PARKED_RETRY_H
#define PARKED_RETRY_H

#include <stdbool.h>
#include <stdint.h>

#include "routing.h"

/* When a parked message's peer may be retried again, having already had one
 * flush attempt that left it parked.
 *
 * Sized against the beacon cadence (BEACON_INTERVAL_MS in mesh_task.c, 60s):
 * five beacons. Retrying on every beacon puts a full ACKed DM round trip on
 * the air every 60s for a peer that keeps refusing it, which is the retry loop
 * the rejoin-only trigger was written to avoid; a beacon is one unacknowledged
 * broadcast and costs far less. Five minutes is also half of
 * NEIGHBOR_EXPIRY_MS (600s), so a peer that goes quiet still gets a second
 * attempt before it ages out of the table and hands the job back to the rejoin
 * edge, and the two triggers never stack up on the same peer.
 *
 * A third constraint pins the floor and is asserted at compile time next to
 * mesh_flush_parked_for: the cooldown must outlast the send queue's TTLs, or a
 * retry could enqueue a row that is still sitting in the queue from the
 * previous attempt and the peer would receive the message twice.
 */
#define PARKED_RETRY_COOLDOWN_MS 300000u

/* How often the node picks one peer with parked messages and tries it,
 * independent of any beacon. Equal to the cooldown and constrained by the same
 * floor (the compile-time assertions next to mesh_flush_parked_for cover both),
 * because a sweep re-queues a DM exactly as a beacon-driven retry does.
 */
#define PARKED_RETRY_SWEEP_MS 300000u

/* The sweep's entire state, owned by the mesh task and touched only there.
 * Zero-initialised means "sweep at the first opportunity", which is what a
 * node wants after a reboot: parked rows persist in flash, nothing is armed
 * yet, and a peer reachable only over a route will never send a beacon to
 * announce itself. */
typedef struct {
    uint32_t next_sweep_ms; /* uptime the next sweep may run at */
    uint32_t last_peer;     /* peer the last sweep looked at; also the rotation cursor */
    uint32_t hold_until_ms; /* until then, a beacon must not re-attempt last_peer */
} parked_sweep_t;

/* True if a sweep may run now, and schedule the next one either way. Consumes
 * the due-ness, so call it once per opportunity: a false keeps the caller off
 * the message store entirely, which is what keeps a node with nothing parked
 * paying nothing. */
bool parked_retry_sweep_due(parked_sweep_t* s, uint32_t now_ms);

/* Record that the sweep ATTEMPTED peer_addr: advances the rotation and holds
 * the beacon path off that peer for one cooldown, so a peer that starts
 * beaconing seconds after a sweep tried it cannot have its rows queued twice. */
void parked_retry_swept(parked_sweep_t* s, uint32_t peer_addr, uint32_t now_ms);

/* Record that the sweep passed OVER peer_addr without attempting it (it is a
 * direct neighbor, so the beacon trigger owns it). Advances the rotation and
 * nothing else: holding a peer the sweep did not touch would suppress the
 * trigger that does own it. */
void parked_retry_sweep_skipped(parked_sweep_t* s, uint32_t peer_addr);

/* Arm peer_addr's neighbor entry so the peer's next beacon re-sends what was
 * just parked for it. Returns false if peer_addr has no entry, which needs no
 * arming: a peer outside the table can only come back by being admitted to it,
 * and that rejoin edge already flushes.
 *
 * A park runs on the UI/RPC caller's task, not the mesh task, so the caller
 * holds the lock that guards cross-task reads of the table (mesh_task's
 * s_state_mutex, the same one mesh_get_peer_name takes).
 */
bool parked_retry_arm(neighbor_table_t* table, uint32_t peer_addr, uint32_t now_ms);

/* Whether a beacon just received from peer_addr should flush that peer's
 * parked messages. True on the rejoin edge (is_new_peer), exactly as before,
 * and also when the peer was armed by a park and its cooldown has elapsed.
 * An unarmed peer costs one table lookup and nothing else: no store scan.
 * Always false while the sweep holds this peer, whichever of the two reasons
 * would otherwise have said yes.
 */
bool parked_retry_beacon_should_flush(neighbor_table_t* table, const parked_sweep_t* sweep,
                                      uint32_t peer_addr, bool is_new_peer, uint32_t now_ms);

/* Record what the flush found, where found is the number of parked rows it
 * picked up. Nothing left parked disarms the peer; anything still parked
 * rearms it one cooldown out. Disarming here rather than at every point a row
 * can leave the parked state (delivery, cancel, eviction) keeps this to one
 * site: the cost of learning lazily is a single store scan, once, and the
 * entry then stays quiet until another park arms it.
 */
void parked_retry_flushed(neighbor_table_t* table, uint32_t peer_addr, int found, uint32_t now_ms);

#endif /* PARKED_RETRY_H */
