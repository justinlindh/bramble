#ifndef BRAMBLE_CHANNEL_FLOOD_H
#define BRAMBLE_CHANNEL_FLOOD_H

#include <stdbool.h>
#include <stdint.h>

#include "packet.h" /* BRAMBLE_MAX_PACKET_SIZE */

typedef struct {
    bool should_relay;
    uint8_t new_hop_limit;
    uint32_t jitter_ms;
} channel_flood_decision_t;

/*
 * The multi-hop channel/broadcast flood relay decision (Phase 1 delivery-
 * core Task 5). Broadcast/channel DATA (dest_addr == 0xFFFFFFFF) is
 * delivered locally today but never rebroadcast, so a group message only
 * ever reaches direct radio neighbors. This is the pure decision that
 * fixes that: whether THIS node, having just received/delivered a
 * broadcast DATA frame, should also relay it onward.
 *
 * This is a fresh design, not a revival of the caller-less channel_flood.c
 * deleted in 703d78a1: that version hardcoded its own flat 50-300ms jitter
 * constants and took a dedup_buffer_t pointer + packet_id directly (doing
 * its own packet_id-only dedup lookup inside the "pure" function, which
 * both made it untestable without a real dedup buffer and reused a
 * packet_id-only key -- the delivery-path audit's flagged cross-source
 * collision risk: two different originators' broadcasts could collide on
 * a 32-bit packet_id and one would be silently dropped as the other's
 * duplicate). This version:
 *   - takes the dup-check RESULT as a plain bool, so the caller decides how
 *     to key its dedup lookup (mesh_task.c uses a src_addr-qualified key
 *     for exactly this reason) and this function stays trivially testable;
 *   - reuses the RREQ forward path's jitter range (RREQ_FWD_JITTER_MIN_MS/
 *     RREQ_FWD_JITTER_MAX_MS, discovery.h) via discovery_forward_jitter_ms
 *     instead of a second hardcoded constant set, so same-hop relays
 *     already avoid keying up simultaneously for RREQ and now for channel
 *     floods too, with one tuning knob instead of two;
 *   - takes the airtime budget's real permit/deny as an explicit input
 *     (budget_permits), so a saturated node stops relaying instead of
 *     amplifying a storm -- the airtime-aware lever the plan calls for.
 *
 * Inputs:
 *   hop_limit      - the RECEIVED header hop_limit (before any decrement).
 *   is_duplicate   - true if the caller's dedup lookup already saw this
 *                     broadcast (src_addr + packet_id, or equivalent).
 *   budget_permits - true if the real airtime budget (BROADCAST lane; see
 *                     tx_gate_can_transmit/tx_gate_check) currently allows
 *                     transmitting a frame of this size. Non-mutating: the
 *                     caller's actual mesh_tx() still performs the real
 *                     check-and-debit at the jittered send time, so this is
 *                     a pre-check, not the final word.
 *   random_value   - caller-supplied randomness (e.g. esp_random()) for the
 *                     jitter draw. Kept as an explicit input, mirroring
 *                     discovery_forward_jitter_ms itself, so this function
 *                     stays pure and deterministic under test.
 *
 * Rules (first match wins):
 *   - hop_limit <= 1 (exhausted -- a relay receiving 1 does not forward,
 *     matching forward_data()/RREQ's ">1" convention, so hop_limit N means
 *     exactly N-hop reach) -> no relay.
 *   - is_duplicate -> no relay (already flooded by/through this node).
 *   - !budget_permits -> no relay (airtime-aware: a saturated node yields).
 *   - otherwise -> relay, with new_hop_limit = hop_limit - 1 and jitter_ms
 *     drawn from the shared RREQ_FWD_JITTER_MIN_MS..RREQ_FWD_JITTER_MAX_MS
 *     range.
 */
channel_flood_decision_t channel_flood_decide(uint8_t hop_limit, bool is_duplicate,
                                              bool budget_permits, uint32_t random_value);

/*
 * Flood-transport origination hop budget (Flooding F1 finalize).
 *
 * The flood transport originates a DATA frame (and the flooded-ACK that
 * confirms it) at an OPERATOR-SETTABLE hop limit so its best-effort reach can
 * be matched to the expected network diameter. 8 (the default, unchanged from
 * the shipped ROUTE_HOP_LIMIT_MAX the flood used to originate at) covers a
 * small/moderate-diameter mesh; a larger value covers a larger-diameter mesh
 * at a documented airtime cost (roughly 3x airtime / many more collisions to
 * cover ~2.5x more hops -- see docs/bramble-protocol-spec.md's flood
 * operating-envelope section). This is a SEPARATE value from
 * ROUTE_HOP_LIMIT_MAX: the reactive routing path still originates and forwards
 * at ROUTE_HOP_LIMIT_MAX unchanged, so raising the flood hop limit never
 * touches reactive reach.
 */
#define FLOOD_HOP_LIMIT_DEFAULT 8
#define FLOOD_HOP_LIMIT_MIN 1
#define FLOOD_HOP_LIMIT_CEIL 32

/*
 * Clamp an operator-supplied flood hop limit into [FLOOD_HOP_LIMIT_MIN,
 * FLOOD_HOP_LIMIT_CEIL]. Pure; shared by the RPC setter, the NVS load, and
 * the origination selector below so all three agree on the valid range.
 */
uint8_t flood_hop_limit_clamp(uint32_t hops);

/*
 * The hop_limit an ORIGINATOR stamps on a freshly-originated frame. Under the
 * flood transport it is the clamped operator-settable flood hop limit;
 * otherwise it is ROUTE_HOP_LIMIT_MAX, the reactive path's unchanged
 * full-depth budget. Shared by send_data_packet / send_dm_packet / send_ack
 * so every originator agrees, and directly unit-testable (it is what proves
 * flood origination uses the configured value, not a constant).
 */
uint8_t flood_origination_hop_limit(bool flood_transport, uint32_t flood_hop_limit);

/*
 * Rebroadcast suppression (Flooding F1). A node that has a flood rebroadcast
 * still waiting out its jitter CANCELS it once it has overheard enough OTHER
 * copies of the same frame from neighbors: those copies already covered the
 * airspace this node's relay would have, so keying up would only add a
 * redundant collision. This is the tuning that makes flooding reliable in
 * small/sparse meshes, and it must match the Go model
 * (simulator/gosim/flood.go, floodSuppressAfterHeard) byte for byte so the
 * firmware flood reproduces the model's measured delivery.
 *
 * FLOOD_SUPPRESS_AFTER is Bramble's threshold: cancel after this many
 * overheard copies. Meshtastic's managed flooding effectively cancels on the
 * FIRST overheard copy (an effective threshold of 1); Bramble uses 2 because
 * in a small/sparse mesh one overheard copy is not yet evidence that every
 * onward neighbor is already covered, and cancelling that eagerly leaves
 * coverage holes. Mirrors flood.go's floodSuppressAfterHeard = 2.
 */
#define FLOOD_SUPPRESS_AFTER 2

/* Capacity of the jittered flood relay queue (see mesh_task.c's
 * s_flood_relay_queue). Kept here alongside pending_flood_relay_t so the
 * suppression helper below can be unit-tested against the real queue type. */
#define FLOOD_RELAY_QUEUE_CAPACITY 8

/*
 * One pending (scheduled-but-not-yet-fired) flood rebroadcast. Holds the
 * exact relay-mutated wire bytes (hop_limit decremented, prev_hop rewritten
 * to us) a broadcast/unicast-flood DATA frame is rebroadcast with once its
 * jitter elapses, plus the suppression bookkeeping:
 *   flood_key = packet_id ^ src_addr, the SAME src-qualified value the
 *               dispatch dedup path recomputes for an overheard copy, so a
 *               duplicate from a different originator never matches this
 *               entry (see channel_flood_note_overheard).
 *   heard     = count of OTHER copies overheard since this relay was queued.
 *   tx_kind   = the tx_gate kind (a tx_kind_t stored as a plain uint8_t to
 *               avoid a routing->radio header dependency) the relay is sent
 *               with, so a flooded DATA debits the BROADCAST lane and a
 *               flooded ACK (Flooding F1 Task 2) debits the CRITICAL/ACK lane
 *               -- one shared queue + suppression engine, correct per-lane
 *               airtime accounting.
 */
typedef struct {
    bool used;
    uint32_t due_at_ms;
    uint8_t buf[BRAMBLE_MAX_PACKET_SIZE];
    uint8_t len;
    uint32_t flood_key;
    uint8_t heard;
    uint8_t tx_kind;
} pending_flood_relay_t;

/*
 * channel_flood_note_overheard: register ONE overheard duplicate copy of
 * flood_key against a queue of pending flood relays and decide whether the
 * matching relay should now be cancelled.
 *
 * flood_key is src-qualified (packet_id ^ src_addr), exactly the value
 * schedule_flood_relay recorded, so a duplicate that originated from a
 * DIFFERENT sender never matches a queued relay and never counts against it.
 * The FIRST copy of a frame is not a duplicate -- it is what queues the relay
 * (via channel_flood_decide) -- and so is never passed here; only the 2nd,
 * 3rd... copies, the ones the dispatch dedup gate catches, reach this. A
 * node's OWN retransmit is a TX not an RX and is likewise never passed here.
 *
 * On the matching used slot: heard++, and once heard reaches
 * FLOOD_SUPPRESS_AFTER the slot is cancelled (used=false) so the relay queue
 * never fires it. Returns true iff this call cancelled a relay. No-op
 * (returns false) when no used slot matches -- e.g. the relay already fired,
 * or this node never queued one for this frame.
 *
 * Pure over its inputs (no globals, no clock), split out from mesh_task.c's
 * queue for the same testability reason channel_flood_decide is.
 */
bool channel_flood_note_overheard(pending_flood_relay_t* queue, int capacity, uint32_t flood_key);

#endif
