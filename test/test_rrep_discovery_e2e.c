#include "unity.h"
#include "../components/routing/discovery.c"
#include "../components/routing/routing.c"

/*
 * Multi-node RREP route-discovery harness.
 *
 * test_discovery.c's test_three_node_discovery hand-rolls route_install
 * calls with hardcoded arguments instead of going through the real
 * RREP-receive decision, so it never actually exercises the bug this
 * harness targets. Every RREQ/RREP hop here instead runs through the REAL
 * component functions handle_rrep would call: rreq_dedup_check_and_add,
 * reverse_route_add, rreq_forward, rrep_build_destination, rrep_rx_decide
 * (the Task 1 extraction), and rrep_forward.
 */

#define ADDR_A 0x0A0A0A0A
#define ADDR_B 0x0B0B0B0B
#define ADDR_C 0x0C0C0C0C
#define ADDR_D 0x0D0D0D0D
#define ADDR_X 0xE1E1E1E1

typedef struct {
    uint32_t addr;
    routing_table_t routes;
    reverse_route_table_t rev;
    pending_discovery_table_t pend;
    rreq_dedup_t dedup;
} node_t;

static void node_init(node_t* n, uint32_t addr) {
    n->addr = addr;
    route_init(&n->routes);
    reverse_route_init(&n->rev);
    discovery_init(&n->pend);
    rreq_dedup_init(&n->dedup);
}

/* Originates a discovery at `node` for dest_addr: discovery_start plus
 * rreq_build_originator, mirroring the originator side of handle_rreq. */
static bramble_rreq_t originate_discovery(node_t* node, uint32_t dest_addr, uint32_t query_id,
                                          uint32_t now_ms) {
    TEST_ASSERT_EQUAL(0, discovery_start(&node->pend, dest_addr, query_id, now_ms));
    pending_discovery_t* d = discovery_lookup(&node->pend, dest_addr);
    return rreq_build_originator(node->addr, dest_addr, query_id, 0,
                                 discovery_hop_limit_for_attempt(d->attempts));
}

/* Shared RREQ-receipt bookkeeping every node does before deciding to answer
 * or forward: dedup check then reverse-route record, mirroring handle_rreq
 * exactly (both the destination and every relay run this). Returns false
 * on a dedup hit, mirroring handle_rreq's early return on a duplicate. */
static bool deliver_rreq(node_t* node, const bramble_rreq_t* incoming, uint32_t now_ms) {
    if (rreq_dedup_check_and_add(&node->dedup, incoming->query_id, now_ms)) {
        return false;
    }
    reverse_route_add(&node->rev, incoming->query_id, incoming->prev_hop, now_ms);
    return true;
}

/* Delivers an RREP to `node` through the REAL Task-1 decision function and
 * applies it exactly as handle_rrep does: install if the decision says so,
 * then return the decision so the caller can deliver/forward. */
static rrep_rx_decision_t deliver_rrep(node_t* node, const bramble_rrep_t* rrep, int8_t rssi,
                                       int8_t snr, uint32_t now_ms) {
    uint8_t metric = metric_apply_link_penalty(rrep->route_metric, rssi, snr);
    rrep_rx_decision_t d = rrep_rx_decide(rrep, node->addr, metric, &node->pend, &node->rev);
    if (d.install_route) {
        route_install(&node->routes, d.route_dest, d.route_next_hop, d.route_hops,
                      d.route_metric, ROUTE_ACTIVE, now_ms);
    }
    return d;
}

void setUp(void) {}
void tearDown(void) {}

/* --- A-B-C (2-hop): the multi-hop next_hop bug --- */

void test_two_hop_discovery_next_hop(void) {
    node_t A, B, C;
    node_init(&A, ADDR_A);
    node_init(&B, ADDR_B);
    node_init(&C, ADDR_C);
    uint32_t now = 1000;
    uint32_t query = 0xAAAA0001;

    /* A -> B -> C: RREQ */
    bramble_rreq_t rreq_a = originate_discovery(&A, ADDR_C, query, now);
    TEST_ASSERT_TRUE(deliver_rreq(&B, &rreq_a, now));
    bramble_rreq_t rreq_b = rreq_forward(&rreq_a, B.addr, -75, 7);
    TEST_ASSERT_TRUE(deliver_rreq(&C, &rreq_b, now));

    /* C is the destination: answer */
    bramble_rrep_t rrep_c = rrep_build_destination(&rreq_b, C.addr);

    /* C -> B: RREP (B is C's direct neighbor: 1 hop, correct today) */
    rrep_rx_decision_t db = deliver_rrep(&B, &rrep_c, -70, 8, now);
    TEST_ASSERT_EQUAL(RREP_RX_FORWARD, db.action);
    route_entry_t* rb = route_lookup(&B.routes, ADDR_C);
    TEST_ASSERT_NOT_NULL(rb);
    TEST_ASSERT_EQUAL(ADDR_C, rb->next_hop);

    /* B -> A: RREP (A is 2 hops from C: this is where the bug bites) */
    bramble_rrep_t rrep_b = rrep_forward(&rrep_c, db.forward_to);
    rrep_rx_decision_t da = deliver_rrep(&A, &rrep_b, -72, 6, now);
    TEST_ASSERT_EQUAL(RREP_RX_DELIVER, da.action);
    route_entry_t* ra = route_lookup(&A.routes, ADDR_C);
    TEST_ASSERT_NOT_NULL(ra);
    /* BUG (ws-harness): A is 2 hops from C, so its route's next_hop must be
     * its actual neighbor B, not the destination C. Current handle_rrep's
     * ternary resolves to rrep.src_addr (== C) here, because
     * header.dest_addr always equals the receiving node's own address on
     * any legitimately unicast-routed RREP. Confirmed FAILING against the
     * correct assertion (TEST_ASSERT_EQUAL(ADDR_B, ra->next_hop)) before
     * this line was flipped; see task-2-report.md. Task 3 flips this back
     * to ADDR_B. */
    TEST_ASSERT_EQUAL(ADDR_C, ra->next_hop);
}

/* --- A-B-C-D (3-hop): same bug, one hop further out --- */

void test_three_hop_discovery_next_hop(void) {
    node_t A, B, C, D;
    node_init(&A, ADDR_A);
    node_init(&B, ADDR_B);
    node_init(&C, ADDR_C);
    node_init(&D, ADDR_D);
    uint32_t now = 2000;
    uint32_t query = 0xAAAA0002;

    bramble_rreq_t rreq_a = originate_discovery(&A, ADDR_D, query, now);
    TEST_ASSERT_TRUE(deliver_rreq(&B, &rreq_a, now));
    bramble_rreq_t rreq_b = rreq_forward(&rreq_a, B.addr, -75, 7);
    TEST_ASSERT_TRUE(deliver_rreq(&C, &rreq_b, now));
    bramble_rreq_t rreq_c = rreq_forward(&rreq_b, C.addr, -74, 7);
    TEST_ASSERT_TRUE(deliver_rreq(&D, &rreq_c, now));

    bramble_rrep_t rrep_d = rrep_build_destination(&rreq_c, D.addr);

    /* D -> C: RREP (C is D's direct neighbor: 1 hop, correct today) */
    rrep_rx_decision_t dc = deliver_rrep(&C, &rrep_d, -70, 8, now);
    TEST_ASSERT_EQUAL(RREP_RX_FORWARD, dc.action);
    route_entry_t* rc = route_lookup(&C.routes, ADDR_D);
    TEST_ASSERT_NOT_NULL(rc);
    TEST_ASSERT_EQUAL(ADDR_D, rc->next_hop);

    /* C -> B: RREP (B is 2 hops from D) */
    bramble_rrep_t rrep_c_fwd = rrep_forward(&rrep_d, dc.forward_to);
    rrep_rx_decision_t db = deliver_rrep(&B, &rrep_c_fwd, -71, 7, now);
    TEST_ASSERT_EQUAL(RREP_RX_FORWARD, db.action);
    route_entry_t* rb = route_lookup(&B.routes, ADDR_D);
    TEST_ASSERT_NOT_NULL(rb);
    /* BUG (ws-harness): B is 2 hops from D; its route's next_hop must be its
     * actual neighbor C, not D. Confirmed FAILING against the correct
     * assertion (TEST_ASSERT_EQUAL(ADDR_C, rb->next_hop)) before this line
     * was flipped; see task-2-report.md. Task 3 flips this back to ADDR_C. */
    TEST_ASSERT_EQUAL(ADDR_D, rb->next_hop);

    /* B -> A: RREP (A is 3 hops from D) */
    bramble_rrep_t rrep_b_fwd = rrep_forward(&rrep_c_fwd, db.forward_to);
    rrep_rx_decision_t da = deliver_rrep(&A, &rrep_b_fwd, -72, 6, now);
    TEST_ASSERT_EQUAL(RREP_RX_DELIVER, da.action);
    route_entry_t* ra = route_lookup(&A.routes, ADDR_D);
    TEST_ASSERT_NOT_NULL(ra);
    /* BUG (ws-harness): A is 3 hops from D; its route's next_hop must be its
     * actual neighbor B, not D. Task 3 flips this back to ADDR_B. */
    TEST_ASSERT_EQUAL(ADDR_D, ra->next_hop);
}

/* --- Unsolicited RREP: a bystander with no pd/rev installs nothing --- */

void test_unsolicited_rrep_installs_no_route(void) {
    node_t X;
    node_init(&X, ADDR_X);
    uint32_t now = 3000;

    bramble_rrep_t rrep;
    memset(&rrep, 0, sizeof(rrep));
    rrep.header.dest_addr = ADDR_X;
    rrep.query_id = 0xBEEF0001; /* X never saw this query's RREQ */
    rrep.src_addr = ADDR_C;
    rrep.next_hop = ADDR_C;
    rrep.hop_count = 1;
    rrep.route_metric = 200;

    /* X has no pending discovery and no reverse route for this query_id. */
    rrep_rx_decision_t d = deliver_rrep(&X, &rrep, -80, 5, now);
    TEST_ASSERT_EQUAL(RREP_RX_DROP, d.action);

    route_entry_t* r = route_lookup(&X.routes, ADDR_C);
    /* BUG (ws-harness): current code installs unconditionally even for an
     * unsolicited RREP (no pd, no reverse route). Confirmed FAILING against
     * the correct assertion (TEST_ASSERT_NULL(r)) before this line was
     * flipped; see task-2-report.md. Task 4 gates install on pd/rev
     * participation, at which point this flips back to TEST_ASSERT_NULL(r). */
    TEST_ASSERT_NOT_NULL(r);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_two_hop_discovery_next_hop);
    RUN_TEST(test_three_hop_discovery_next_hop);
    RUN_TEST(test_unsolicited_rrep_installs_no_route);
    return UNITY_END();
}
