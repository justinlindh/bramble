/*
 * Phase 1 delivery-core plan, Task 4: end-to-end proof that wire v4's
 * relay-mutated prev_hop plus data_rx_decide's reverse-route learning
 * actually closes the confirmation-return bug (THE BUG: relays only ever
 * installed routes TOWARD discovery targets, never back toward a message's
 * ORIGINATOR, so a destination's ACK/receipt died at route_lookup(source)
 * == NULL at the first relay).
 *
 * This drives the REAL functions across a 3-node route table (A -> B -> C):
 *   - data_rx_decide (components/routing/forwarding.c): the same decision
 *     mesh_process_rx_packet's PKT_TYPE_DATA case calls.
 *   - route_install (components/routing/routing.c): the same install every
 *     other route source (RREQ/RREP) goes through.
 *   - forward_data (components/routing/forwarding.c): the same
 *     route-lookup + hop-limit-decrement decision forward_data_packet
 *     calls, used here twice: once to move A's DATA from B toward C
 *     (mirroring B's own forward step, so C sees the correctly decremented
 *     hop_limit and thus the correctly derived hop count), and once to
 *     prove C's return ACK/receipt now finds a route home at B.
 *
 * No mesh_task.c/gosim glue is exercised here (that is main/mesh_task.c and
 * simulator/gosim/bridge.c's job); this is the pure protocol-logic layer.
 */
#include "unity.h"
#include "../components/routing/forwarding.c"
#include "../components/routing/routing.c"

#define ADDR_A 0x0A0A0A0Au
#define ADDR_B 0x0B0B0B0Bu
#define ADDR_C 0x0C0C0C0Cu

typedef struct {
    uint32_t addr;
    routing_table_t routes;
} node_t;

static void node_init(node_t* n, uint32_t addr) {
    n->addr = addr;
    route_init(&n->routes);
}

/* Runs the same decision + install sequence mesh_process_rx_packet's
 * PKT_TYPE_DATA case runs: compute the link metric the way handle_rrep's
 * caller does (metric_apply_link_penalty over a 255 base, since DATA has no
 * propagated path metric of its own), call data_rx_decide, and install the
 * reverse route if the decision says so. Returns the decision so the
 * caller can also act on `action` (deliver vs forward). */
static data_rx_decision_t receive_data(node_t* node, uint32_t dest_addr, uint32_t src_addr,
                                       uint32_t prev_hop, uint8_t received_hop_limit, int8_t rssi,
                                       int8_t snr, uint32_t now_ms) {
    uint8_t link_metric = metric_apply_link_penalty(255, rssi, snr);
    data_rx_decision_t d =
        data_rx_decide(dest_addr, node->addr, src_addr, prev_hop, received_hop_limit, link_metric);
    if (d.install_reverse_route) {
        route_install(&node->routes, d.reverse_dest, d.reverse_next_hop, d.reverse_hop_count,
                      d.reverse_metric, ROUTE_ACTIVE, ROUTE_SRC_BREADCRUMB, now_ms);
    }
    return d;
}

void setUp(void) {}
void tearDown(void) {}

/*
 * A -> B -> C unicast DATA. B already has a (pre-discovered) forward route
 * to C; A and C do not need a route to reach each other for this test, only
 * the REVERSE breadcrumbs this scenario is proving.
 */
void test_data_transit_installs_reverse_routes_and_ack_forwards_home(void) {
    node_t A, B, C;
    node_init(&A, ADDR_A);
    node_init(&B, ADDR_B);
    node_init(&C, ADDR_C);
    uint32_t now = 5000;

    /* Simulate prior discovery: B already knows how to reach C directly. */
    route_install(&B.routes, ADDR_C, ADDR_C, 1, 200, ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, now);

    /* --- Hop 1: A originates, B receives --- */
    /* A writes its own address as both src_addr and prev_hop (send_data_packet's
     * ORIGINATOR behavior) and sends at the full hop budget. */
    uint8_t hop_limit_at_b = ROUTE_HOP_LIMIT_MAX;
    data_rx_decision_t db = receive_data(&B, ADDR_C, ADDR_A, ADDR_A, hop_limit_at_b, -70, 8, now);
    TEST_ASSERT_EQUAL(DATA_RX_FORWARD, db.action);

    /* B must have learned a route back to A, dest=A via next_hop=A (A is
     * B's direct neighbor: 1 hop). This is THE fix: B never had this route
     * before wire v4. */
    route_entry_t* rb_to_a = route_lookup(&B.routes, ADDR_A);
    TEST_ASSERT_NOT_NULL(rb_to_a);
    TEST_ASSERT_EQUAL_UINT32(ADDR_A, rb_to_a->next_hop);
    TEST_ASSERT_EQUAL(1, rb_to_a->hop_count);

    /* --- B forwards toward C: same forward_data() forward_data_packet calls --- */
    uint8_t hop_limit_for_forward = hop_limit_at_b;
    forward_result_t fwd = forward_data(&B.routes, ADDR_C, &hop_limit_for_forward, now);
    TEST_ASSERT_TRUE(fwd.should_send);
    TEST_ASSERT_EQUAL_UINT32(ADDR_C, fwd.next_hop);
    /* forward_data_packet's wire-v4 rewrite: B overwrites prev_hop with its
     * own address before rebroadcast. */
    uint32_t prev_hop_from_b = ADDR_B;

    /* --- Hop 2: C receives (dest_addr == self: DELIVER) --- */
    data_rx_decision_t dc =
        receive_data(&C, ADDR_C, ADDR_A, prev_hop_from_b, hop_limit_for_forward, -68, 9, now);
    TEST_ASSERT_EQUAL(DATA_RX_DELIVER, dc.action);

    /* C must have learned a route back to A, dest=A via next_hop=B (the
     * verified last radio hop), at 2 hops: exactly the A-B-C hop-count
     * derivation from the brief (hops_traveled = ROUTE_HOP_LIMIT_MAX -
     * received_hop_limit + 1). */
    route_entry_t* rc_to_a = route_lookup(&C.routes, ADDR_A);
    TEST_ASSERT_NOT_NULL(rc_to_a);
    TEST_ASSERT_EQUAL_UINT32(ADDR_B, rc_to_a->next_hop);
    TEST_ASSERT_EQUAL(2, rc_to_a->hop_count);

    /*
     * THE PAYOFF: C's delivery confirmation (ACK/receipt) travels back
     * toward A. At B, forward_data(&B.routes, A, ...) must now find the
     * route this scenario just installed, instead of the pre-fix
     * route_lookup(A) == NULL that silently dropped it in forward_ack.
     */
    uint8_t ack_hop_limit = ROUTE_HOP_LIMIT_MAX;
    forward_result_t ack_fwd = forward_data(&B.routes, ADDR_A, &ack_hop_limit, now);
    TEST_ASSERT_TRUE_MESSAGE(ack_fwd.should_send,
                             "B must be able to forward C's confirmation back toward A: "
                             "this is the confirmation-return bug wire v4 fixes");
    TEST_ASSERT_FALSE(ack_fwd.route_error);
    TEST_ASSERT_EQUAL_UINT32(ADDR_A, ack_fwd.next_hop);
}

/*
 * Guard: a node must never learn a route to itself. If B somehow receives a
 * frame whose src_addr or prev_hop is its own address (an echo / loopback),
 * data_rx_decide must not have flagged an install (already covered at the
 * unit level in test_forwarding.c); this test additionally proves that when
 * install_reverse_route is (correctly) false, mesh_task/gosim's "only call
 * route_install when the decision says so" convention leaves the routing
 * table untouched.
 */
void test_self_referential_data_never_installs_a_route(void) {
    node_t B;
    node_init(&B, ADDR_B);
    uint32_t now = 6000;

    data_rx_decision_t d =
        receive_data(&B, ADDR_C, ADDR_B, ADDR_A, ROUTE_HOP_LIMIT_MAX, -70, 8, now);
    TEST_ASSERT_FALSE(d.install_reverse_route);
    TEST_ASSERT_NULL(route_lookup(&B.routes, ADDR_B));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_data_transit_installs_reverse_routes_and_ack_forwards_home);
    RUN_TEST(test_self_referential_data_never_installs_a_route);
    return UNITY_END();
}
