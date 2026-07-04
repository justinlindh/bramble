#include "include/forwarding.h"
#include <string.h>

#define BROKEN_THRESHOLD 3

forward_result_t forward_data(routing_table_t* table, uint32_t dest_addr, uint8_t* hop_limit,
                              uint32_t now_ms) {
    forward_result_t res = {0, false, false};

    if (*hop_limit <= 1) {
        res.should_send = false;
        return res;
    }

    route_entry_t* r = route_lookup(table, dest_addr);
    if (!r || r->state == ROUTE_BROKEN) {
        res.route_error = true;
        return res;
    }

    /* Promote STALE → ACTIVE */
    if (r->state == ROUTE_STALE) {
        r->state = ROUTE_ACTIVE;
        r->last_confirmed = now_ms;
    }

    r->last_used = now_ms;
    r->use_count++;
    (*hop_limit)--;
    res.next_hop = r->next_hop;
    res.should_send = true;
    return res;
}

void forward_record_failure(routing_table_t* table, uint32_t dest_addr) {
    route_entry_t* r = route_lookup(table, dest_addr);
    if (!r)
        return;
    r->fail_count++;
    if (r->fail_count >= BROKEN_THRESHOLD) {
        r->state = ROUTE_BROKEN;
    }
}

bramble_rerr_t rerr_build(uint32_t my_addr, uint32_t broken_dest, uint32_t broken_next_hop) {
    bramble_rerr_t e;
    memset(&e, 0, sizeof(e));
    e.header.version = BRAMBLE_VERSION;
    e.header.type = PKT_TYPE_RERR;
    e.header.flags = 0;
    /* Per-hop budget only: relays that match the broken route re-originate
     * the RERR with a fresh hop limit, so the route-match chain (not this
     * value) bounds how far a teardown propagates. */
    e.header.hop_limit = ROUTE_HOP_LIMIT_MAX;
    e.header.dest_addr = 0xFFFFFFFF;
    e.header.packet_id = 0;
    e.reporter_addr = my_addr;
    e.broken_dest = broken_dest;
    e.broken_next_hop = broken_next_hop;
    return e;
}

bool rerr_handle(routing_table_t* table, const bramble_rerr_t* rerr) {
    route_entry_t* r = route_lookup(table, rerr->broken_dest);
    if (r && r->next_hop == rerr->broken_next_hop) {
        r->state = ROUTE_BROKEN;
        r->fail_count++;
        return true;
    }
    return false;
}

data_rx_decision_t data_rx_decide(uint32_t dest_addr, uint32_t self_addr) {
    data_rx_decision_t d;
    memset(&d, 0, sizeof(d));

    /* Reverse-route learning waits on wire v4's prev_hop field (Task 4);
     * until then this is always false, matching today's behavior exactly. */
    d.install_reverse_route = false;
    d.reverse_dest = 0;
    d.reverse_next_hop = 0;

    if (dest_addr == self_addr || dest_addr == 0xFFFFFFFF) {
        d.action = DATA_RX_DELIVER;
    } else {
        d.action = DATA_RX_FORWARD;
    }
    return d;
}
