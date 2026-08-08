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
 * Purely an airtime knob: nothing about correctness rests on the number. A
 * retry cannot duplicate a message however soon it lands, because the send
 * queue holds at most one entry per uid (see mesh_flush_parked_for).
 */
#define PARKED_RETRY_COOLDOWN_MS 300000u

/* How often the node picks one peer with parked messages and tries it,
 * independent of any beacon. An airtime knob on the same terms as the cooldown
 * above, and for the same reason: a sweep re-queues a DM exactly as a
 * beacon-driven retry does, and the send queue refuses a uid it already holds
 * however the second attempt got there.
 *
 * Set equal to the cooldown because the two pace the same thing, not because
 * anything requires it. Nothing reads the difference: the sweep's hold is
 * cleared when the rotation moves off the peer that earned it
 * (parked_retry_sweep_skipped), so tuning these apart cannot leak one peer's
 * hold onto another.
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
 * direct neighbor with a live trigger of its own, so the beacon path owns it).
 * Advances the rotation and clears any hold: holding a peer the sweep did not
 * touch would suppress the trigger that does own it. */
void parked_retry_sweep_skipped(parked_sweep_t* s, uint32_t peer_addr);

/* Whether the sweep should leave peer_addr to the beacon trigger. True only
 * for a neighbor that is actually armed. A neighbor with parked rows and an
 * UNARMED entry is an anomaly with no other way out: the beacon path will not
 * flush it (nothing armed, and a peer that keeps beaconing is never admitted
 * again to fire the rejoin edge), and the sweep passing over every neighbor on
 * principle would leave it stranded forever under a promise this node made.
 *
 * Two ways to reach that state, one of them ordinary: an entry evicted from a
 * full table and readmitted comes back zeroed, and the park that armed it is
 * long past. The other is the arming write racing the mesh task's own
 * neighbor-table writes. Neither is worth a special case of its own, and the
 * check below covers any third way nobody has thought of, so the sweep treats
 * "parked rows but no arming" as the anomaly it is and takes the peer. */
bool parked_retry_sweep_defers_to_beacon(neighbor_table_t* table, uint32_t peer_addr);

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

/* Decide whether a beacon just received from peer_addr should flush that
 * peer's parked messages, and record the consequences of that decision.
 *
 * MESH TASK ONLY, and it WRITES the table. Not a query despite the question it
 * answers: when the sweep holds this peer, the decision is to defer rather
 * than to drop, and deferring means arming the entry for when the hold ends.
 * The name says decide, not ask, for exactly that reason.
 *
 * Because it writes, its caller holds the lock that guards writes to
 * s_neighbors: in the firmware it is reached only through
 * mesh_parked_retry_decide_flush_locked (mesh_internal.h), never directly.
 *
 * Flushes on the rejoin edge (is_new_peer), and when the peer was armed by a
 * park and its cooldown has elapsed. An unarmed peer costs one table lookup
 * and nothing else: no store scan.
 */
bool parked_retry_beacon_decide_flush(neighbor_table_t* table, const parked_sweep_t* sweep,
                                      uint32_t peer_addr, bool is_new_peer, uint32_t now_ms);

/* Record what the flush found, where found is the number of parked rows it
 * picked up. Writes the table, on the same terms as the decision above: mesh
 * task only, reached through mesh_parked_retry_flushed_locked.
 *
 * Nothing left parked disarms the peer; anything still parked
 * rearms it one cooldown out. Disarming here rather than at every point a row
 * can leave the parked state (delivery, cancel, eviction) keeps this to one
 * site: the cost of learning lazily is a single store scan, once, and the
 * entry then stays quiet until another park arms it.
 */
void parked_retry_flushed(neighbor_table_t* table, uint32_t peer_addr, int found, uint32_t now_ms);

#endif /* PARKED_RETRY_H */
