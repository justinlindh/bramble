#ifndef BRAMBLE_DISCOVERY_H
#define BRAMBLE_DISCOVERY_H

#include "packet.h"
#include "routing.h"
#include <stdbool.h>

#define MAX_PENDING_DISCOVERIES 8
#define RREQ_RETRY_INTERVAL_1_MS 5000
#define RREQ_RETRY_INTERVAL_2_MS 15000
#define MAX_RREQ_ATTEMPTS 3

/* Expanding-ring discovery: the first attempt floods with a conservative hop
 * budget; retries widen to the protocol's maximum route depth. Each retry
 * carries a fresh query_id (and therefore a fresh originator pseudonym), so
 * retries are not swallowed by the RREQ dedup window on nodes that heard an
 * earlier attempt. */
#define RREQ_HOP_LIMIT_INITIAL 4
#define RREQ_HOP_LIMIT_EXPANDED ROUTE_HOP_LIMIT_MAX

/* Relays delay RREQ rebroadcast by a random jitter in this range so same-hop
 * relays do not key up at the same instant. */
#define RREQ_FWD_JITTER_MIN_MS 50
#define RREQ_FWD_JITTER_MAX_MS 300

typedef struct {
    uint32_t dest_addr;
    uint32_t query_ids[MAX_RREQ_ATTEMPTS]; /* one fresh query_id per attempt */
    uint32_t timestamp;
    uint8_t attempts;
} pending_discovery_t;

typedef struct {
    pending_discovery_t entries[MAX_PENDING_DISCOVERIES];
    int count;
} pending_discovery_table_t;

void discovery_init(pending_discovery_table_t* table);
int discovery_start(pending_discovery_table_t* table, uint32_t dest_addr, uint32_t query_id,
                    uint32_t now_ms);
pending_discovery_t* discovery_lookup(pending_discovery_table_t* table, uint32_t dest_addr);
/* Matches an RREP against ANY query_id of any outstanding attempt, so a late
 * answer to an earlier attempt still completes the discovery. */
pending_discovery_t* discovery_lookup_by_query(pending_discovery_table_t* table, uint32_t query_id);
void discovery_remove(pending_discovery_table_t* table, uint32_t dest_addr);
bool discovery_should_retry(const pending_discovery_t* d, uint32_t now_ms);
/* Records a retry attempt under a freshly generated query_id. */
void discovery_record_attempt(pending_discovery_t* d, uint32_t query_id, uint32_t now_ms);
uint32_t discovery_current_query_id(const pending_discovery_t* d);
/* Expanding ring: attempt 1 uses RREQ_HOP_LIMIT_INITIAL, retries use
 * RREQ_HOP_LIMIT_EXPANDED. */
uint8_t discovery_hop_limit_for_attempt(uint8_t attempt);
/* Maps a random value into [RREQ_FWD_JITTER_MIN_MS, RREQ_FWD_JITTER_MAX_MS]. */
uint32_t discovery_forward_jitter_ms(uint32_t random_value);

bramble_rreq_t rreq_build_originator(uint32_t my_addr, uint32_t dest_addr, uint32_t query_id,
                                     uint32_t encrypted_source, uint8_t hop_limit);
bramble_rreq_t rreq_forward(const bramble_rreq_t* incoming, uint32_t my_addr, int8_t rx_rssi,
                            int8_t rx_snr);
bramble_rrep_t rrep_build_destination(const bramble_rreq_t* rreq, uint32_t my_addr);
/* next_hop_back is the frame-routing target (header.dest_addr): the next
 * physical node this RREP unicasts to, toward the originator. my_addr is
 * THIS relay's own address, written into next_hop so the receiver installs
 * a route via the node that actually delivered the RREP (fixes multi-hop
 * next_hop; see rrep_rx_decide). */
bramble_rrep_t rrep_forward(const bramble_rrep_t* incoming, uint32_t next_hop_back,
                            uint32_t my_addr);

/*
 * SEC-H1 (Task 3.2, STAGED, NOT closed: see network_key.h). Authenticates
 * exactly the 4 origin-stable fields a destination computes once
 * (query_id, src_addr, hop_count, route_metric), deliberately excluding
 * next_hop and header.dest_addr, the only two fields rrep_forward mutates
 * on each relay hop. rrep_sign fills r->auth_hmac; call it once, at the
 * end of rrep_build_destination. rrep_verify recomputes the same MAC and
 * constant-time-compares; returns nonzero (true) iff it matches. With the
 * unprovisioned public-PSK fallback key (network_key_get), this MAC is
 * forgeable by anyone who knows that public constant: it does NOT close
 * SEC-H1 on its own, closure waits on real key provisioning.
 */
/* Fail-closed (mandatory-provisioning Task 2): rrep_sign returns 0 on success
 * and nonzero when UNPROVISIONED (emits the all-zero sentinel, do not send);
 * rrep_verify checks that return and REJECTS before the compare, so an
 * unprovisioned verifier never accepts a frame (never matches the sentinel). */
int rrep_sign(bramble_rrep_t* r);
int rrep_verify(const bramble_rrep_t* r);

/*
 * Phase 2 "save reactive routing": intermediate-node RREP (classic AODV
 * shortcut, RFC 3561 6.6.2). Every RREQ flooding the whole mesh to find its
 * destination is reactive routing's dominant airtime cost at scale
 * (internal-planning plans/2026-07-04-phase2-scale-framework.md). If a relay
 * that receives an RREQ already holds a route to the destination, it can
 * answer on the destination's behalf instead of only forwarding the flood
 * further, short-circuiting discovery for the whole subtree beyond it.
 *
 * AODV normally gates this on a destination sequence number carried in the
 * route (a monotonic per-destination freshness counter): a route is only
 * used to reply if its sequence number is at least as new as the one in
 * the RREQ. Bramble's route_entry_t has NO destination sequence number
 * (route_source_t's doc comment: metric/hop_count/state only), so that
 * exact mechanism is unavailable here. intermediate_rrep_route_usable is
 * deliberately more conservative than AODV to compensate:
 *
 *   - trust class: ONLY a ROUTE_SRC_DISCOVERED route qualifies (installed
 *     from an RREQ/RREP/beacon, HMAC-gated control plane). A
 *     ROUTE_SRC_BREADCRUMB route is an unauthenticated DATA-forwarding
 *     hint (route_source_t's doc comment: prev_hop is relay-mutable and
 *     MAC-excluded) and must never be used to author a reply on someone
 *     else's behalf -- that would let a bystander plant/replay a
 *     breadcrumb-looking path and turn every relay into an open blackhole
 *     oracle for any destination it names.
 *   - state: ONLY ROUTE_ACTIVE qualifies; STALE/BROKEN routes are excluded
 *     (a route heading toward eviction or already known-dead must not be
 *     used to vouch for a path to anyone).
 *   - freshness: last_confirmed must be within INTERMEDIATE_RREP_MAX_AGE_MS,
 *     tighter than ROUTE_ACTIVE_TIMEOUT_MS (5 min). The active/stale state
 *     transition alone is too coarse a freshness signal to stand in for a
 *     destination sequence number: this is a second, much narrower
 *     staleness gate layered on top of it, trading some intermediate-reply
 *     coverage (a route that is still ACTIVE but older than this window
 *     will not be used) for meaningfully lower blackhole/stale-route risk.
 *     A wrong intermediate reply, unlike a wrong forward, actively
 *     terminates the real flood's chance of reaching the true destination
 *     down that subtree, so this errs conservative.
 */
#define INTERMEDIATE_RREP_MAX_AGE_MS 60000

bool intermediate_rrep_route_usable(const route_entry_t* route, uint32_t now_ms);

/*
 * Builds an RREP answering on route_to_dest->dest_addr's behalf, using a
 * cached route instead of being that destination. Mirrors
 * rrep_build_destination's conventions:
 *   - src_addr is the DESTINATION's address (route_to_dest->dest_addr), not
 *     my_addr: this relay is answering FOR that destination.
 *   - next_hop is my_addr: this relay is the first hop back toward D from
 *     its own perspective, exactly like rrep_build_destination's "the
 *     destination is its own first hop toward itself" (rrep_forward
 *     rewrites next_hop at every further relay hop, same as any RREP).
 *   - header.dest_addr is rreq->prev_hop: unicast back toward the RREQ's
 *     sender, same frame-routing target rrep_build_destination uses.
 *   - hop_count is the FULL accumulated path length: hops from the
 *     original RREQ originator to THIS node (rreq->hop_count + 1, the same
 *     +1 rrep_build_destination applies for the final hop into a
 *     destination) plus route_to_dest->hop_count (this node's own cached
 *     distance to D).
 *   - route_metric composes two independently-255-based path-quality
 *     scores (see metric_apply_link_penalty: metric is 255 minus
 *     accumulated link penalties, never an average or a min): metric_to_me
 *     extends the RREQ's accumulated originator->prev_hop score across the
 *     link into this node (exactly what rreq_forward would compute),
 *     route_to_dest->metric already represents 255 minus this node's own
 *     accumulated penalty out to D. Composing them subtracts the second
 *     segment's penalty (255 - route_to_dest->metric) from the first
 *     segment's running score, floored at zero.
 *
 * rx_rssi/rx_snr are this node's OWN reception quality for the RREQ it is
 * answering (the link from rreq->prev_hop into this node), exactly the
 * values handle_rreq already has on hand for the ordinary forward path.
 * Caller must draw a fresh seq (control_seq_next) and re-sign
 * (rrep_sign) before transmitting, exactly like rrep_build_destination's
 * caller does: this builder signs once with seq=0 so it is never
 * transmitted un-signed, but the seq drawn here is not the one that ships.
 */
bramble_rrep_t rrep_build_intermediate(const bramble_rreq_t* rreq,
                                       const route_entry_t* route_to_dest, uint32_t my_addr,
                                       int8_t rx_rssi, int8_t rx_snr);

/* The routing decision an RREP receipt resolves to. Pure and host-testable:
 * operates only on the already-host-testable routing tables, taking crypto
 * verification (rrep_verify, control_replay_ok) as done by the caller.
 *
 * route_next_hop is rrep->next_hop directly: the node that actually
 * delivered this RREP, correct at any hop count (see rrep_forward). This
 * replaced an earlier dest_addr==self_addr ternary that was only correct
 * one hop from the destination (see the harness design doc).
 *
 * install_route is gated on discovery participation: true when we
 * originated the matching RREQ (pd found) or relayed it (rev found), false
 * when neither, so an unsolicited RREP (overheard, or forged for a query we
 * never saw) cannot plant a route. */
typedef enum {
    RREP_RX_DROP = 0,
    RREP_RX_DELIVER,
    RREP_RX_FORWARD,
} rrep_rx_action_t;

typedef struct {
    rrep_rx_action_t action;
    bool install_route;
    uint32_t route_dest;
    uint32_t route_next_hop;
    uint8_t route_hops;
    uint8_t route_metric;
    uint32_t forward_to;   /* RREP_RX_FORWARD: reverse-route prev_hop */
    uint32_t deliver_dest; /* RREP_RX_DELIVER: pd->dest_addr to flush */
} rrep_rx_decision_t;

/* self_addr, link_metric (already link-penalized by the caller), and the
 * node's pending-discovery / reverse-route tables. */
rrep_rx_decision_t rrep_rx_decide(const bramble_rrep_t* rrep, uint32_t self_addr,
                                  uint8_t link_metric, pending_discovery_table_t* pd,
                                  reverse_route_table_t* rev);

#endif
