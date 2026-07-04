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

#endif
