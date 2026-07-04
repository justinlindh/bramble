#ifndef BRAMBLE_FORWARDING_H
#define BRAMBLE_FORWARDING_H

#include "packet.h"
#include "routing.h"
#include <stdbool.h>

typedef struct {
    uint32_t next_hop;
    bool should_send;
    bool route_error;
} forward_result_t;

forward_result_t forward_data(routing_table_t* table, uint32_t dest_addr, uint8_t* hop_limit,
                              uint32_t now_ms);
void forward_record_failure(routing_table_t* table, uint32_t dest_addr);

bramble_rerr_t rerr_build(uint32_t my_addr, uint32_t broken_dest, uint32_t broken_next_hop);

/* Marks the route to broken_dest as ROUTE_BROKEN (and bumps its fail_count)
 * when the route's current next_hop matches broken_next_hop. Returns true
 * when a route was actually marked broken, so a caller can decide whether
 * to re-originate the RERR further (mesh_task.c's handle_rerr does this;
 * gosim's bridge.c does not need the return value). */
bool rerr_handle(routing_table_t* table, const bramble_rerr_t* rerr);

/* The routing decision a received DATA frame resolves to: deliver it to this
 * node's own application layer, or forward it toward dest_addr. Pure and
 * host-testable, mirroring mesh_task.c's mesh_process_rx_packet DATA case
 * EXACTLY (dest_addr == self_addr or the broadcast address 0xFFFFFFFF
 * delivers locally; any other dest_addr forwards).
 *
 * Wire v4 (Task 4 of the Phase 1 delivery-core plan): DATA now carries a
 * relay-mutated prev_hop field (packet.h, BRAMBLE_DATA_PREV_HOP_OFFSET), so
 * this function also decides whether to learn a route back to the DATA's
 * ORIGINATOR: dest = src_addr (AAD-bound, trustworthy), next_hop = prev_hop
 * (the verified last radio hop, unauthenticated/relay-mutable by design).
 * This is what leaves every relay on the forward path a fresh breadcrumb
 * route home, so a destination's ACK/receipt has somewhere to go instead of
 * dying at route_lookup(src_addr) == NULL.
 *
 * install_reverse_route fires for received AND forwarded unicast DATA, and
 * for broadcast DATA too (a broadcast's sender is just as reachable via
 * prev_hop as a unicast sender is) -- it does not depend on `action`.  It
 * does NOT fire when src_addr == self_addr (an echo of our own packet) or
 * prev_hop == self_addr (the last hop was somehow ourselves); either would
 * install a self-referential route.
 *
 * reverse_hop_count is derived from received_hop_limit: the originator
 * always sends at ROUTE_HOP_LIMIT_MAX, and every forwarder decrements
 * hop_limit by exactly one before retransmitting (forward_data). So
 * hops_traveled = ROUTE_HOP_LIMIT_MAX - received_hop_limit + 1. Verified
 * A-B-C: B receives hop_limit == ROUTE_HOP_LIMIT_MAX (A never decremented
 * it) -> 1 hop; C receives hop_limit == ROUTE_HOP_LIMIT_MAX - 1 (B
 * decremented once before forwarding) -> 2 hops.
 *
 * reverse_metric mirrors how RREQ/RREP install a route's metric
 * (metric_apply_link_penalty over a higher-is-better base): DATA carries no
 * accumulated path metric of its own (unlike RREQ/RREP's propagated metric
 * field), so callers pass the same maximum base (255) RREQ originates with,
 * penalized by the ONE link this frame was just heard on (link_metric is
 * precomputed by the caller, mirroring handle_rrep's
 * metric_apply_link_penalty-before-rrep_rx_decide pattern in mesh_task.c).
 * The result reflects only the immediate radio link, not the full path back
 * to the originator -- the best information a breadcrumb route can have
 * without a propagated metric field on the wire. */
typedef enum {
    DATA_RX_DELIVER = 0,
    DATA_RX_FORWARD,
} data_rx_action_t;

typedef struct {
    data_rx_action_t action;
    bool install_reverse_route;
    uint32_t reverse_dest;     /* src_addr: the DATA's originator */
    uint32_t reverse_next_hop; /* prev_hop: the verified last radio hop */
    uint8_t reverse_hop_count; /* derived from received_hop_limit */
    uint8_t reverse_metric;    /* caller-supplied link_metric, passed through */
} data_rx_decision_t;

data_rx_decision_t data_rx_decide(uint32_t dest_addr, uint32_t self_addr, uint32_t src_addr,
                                  uint32_t prev_hop, uint8_t received_hop_limit,
                                  uint8_t link_metric);

#endif
