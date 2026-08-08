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
 */
#define PARKED_RETRY_COOLDOWN_MS 300000u

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
 */
bool parked_retry_beacon_should_flush(neighbor_table_t* table, uint32_t peer_addr, bool is_new_peer,
                                      uint32_t now_ms);

/* Record what the flush found, where found is the number of parked rows it
 * picked up. Nothing left parked disarms the peer; anything still parked
 * rearms it one cooldown out. Disarming here rather than at every point a row
 * can leave the parked state (delivery, cancel, eviction) keeps this to one
 * site: the cost of learning lazily is a single store scan, once, and the
 * entry then stays quiet until another park arms it.
 */
void parked_retry_flushed(neighbor_table_t* table, uint32_t peer_addr, int found, uint32_t now_ms);

#endif /* PARKED_RETRY_H */
