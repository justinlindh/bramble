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
 * install_reverse_route / reverse_dest / reverse_next_hop are placeholders
 * for wire v4 (Task 4 of the Phase 1 delivery-core plan): once DATA carries
 * a relay-mutated prev_hop field, this function will learn a route back to
 * the DATA's originator (dest_addr = the packet's src_addr, next_hop =
 * prev_hop, the verified last radio hop) so ACKs/receipts have a reverse
 * path at every relay. That input does not exist on the wire yet, so today
 * install_reverse_route is unconditionally false; the fields are already
 * part of the struct so Task 4 is a semantic change to this function's body,
 * not an interface change for its callers. */
typedef enum {
    DATA_RX_DELIVER = 0,
    DATA_RX_FORWARD,
} data_rx_action_t;

typedef struct {
    data_rx_action_t action;
    bool install_reverse_route; /* always false today; see Task 4 */
    uint32_t reverse_dest;      /* unused today */
    uint32_t reverse_next_hop;  /* unused today */
} data_rx_decision_t;

data_rx_decision_t data_rx_decide(uint32_t dest_addr, uint32_t self_addr);

#endif
