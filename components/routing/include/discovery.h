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
void rrep_sign(bramble_rrep_t* r);
int rrep_verify(const bramble_rrep_t* r);

/* The routing decision an RREP receipt resolves to. Pure and host-testable:
 * operates only on the already-host-testable routing tables, taking crypto
 * verification (rrep_verify, control_replay_ok) as done by the caller.
 *
 * route_next_hop is rrep->next_hop directly: the node that actually
 * delivered this RREP, correct at any hop count (see rrep_forward). This
 * replaced an earlier dest_addr==self_addr ternary that was only correct
 * one hop from the destination (see the harness design doc). install_route
 * is still unconditionally true; the unsolicited-RREP gate on pd/rev
 * participation is a separate, later fix. */
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
