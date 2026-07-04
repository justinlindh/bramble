#include "unity.h"
#include "../components/routing/forwarding.c"
#include "../components/routing/routing.c"

static routing_table_t rt;

void setUp(void) { route_init(&rt); }
void tearDown(void) {}

void test_forward_active_route(void) {
    route_install(&rt, 0xCCCC, 0xBBBB, 2, 200, ROUTE_ACTIVE, 1000);
    uint8_t hl = 4;
    forward_result_t r = forward_data(&rt, 0xCCCC, &hl, 2000);
    TEST_ASSERT_TRUE(r.should_send);
    TEST_ASSERT_FALSE(r.route_error);
    TEST_ASSERT_EQUAL(0xBBBB, r.next_hop);
    TEST_ASSERT_EQUAL(3, hl);
}

void test_forward_stale_promotion(void) {
    route_install(&rt, 0xCCCC, 0xBBBB, 2, 200, ROUTE_STALE, 1000);
    uint8_t hl = 4;
    forward_result_t r = forward_data(&rt, 0xCCCC, &hl, 2000);
    TEST_ASSERT_TRUE(r.should_send);
    route_entry_t* e = route_lookup(&rt, 0xCCCC);
    TEST_ASSERT_EQUAL(ROUTE_ACTIVE, e->state);
}

void test_forward_ttl_expiry(void) {
    route_install(&rt, 0xCCCC, 0xBBBB, 2, 200, ROUTE_ACTIVE, 1000);
    uint8_t hl = 1;
    forward_result_t r = forward_data(&rt, 0xCCCC, &hl, 2000);
    TEST_ASSERT_FALSE(r.should_send);
    TEST_ASSERT_FALSE(r.route_error);
}

void test_forward_unknown_dest(void) {
    uint8_t hl = 4;
    forward_result_t r = forward_data(&rt, 0xDDDD, &hl, 2000);
    TEST_ASSERT_FALSE(r.should_send);
    TEST_ASSERT_TRUE(r.route_error);
}

void test_forward_broken_after_failures(void) {
    route_install(&rt, 0xCCCC, 0xBBBB, 2, 200, ROUTE_ACTIVE, 1000);
    forward_record_failure(&rt, 0xCCCC);
    forward_record_failure(&rt, 0xCCCC);
    forward_record_failure(&rt, 0xCCCC);
    route_entry_t* e = route_lookup(&rt, 0xCCCC);
    TEST_ASSERT_EQUAL(ROUTE_BROKEN, e->state);
    /* Now forwarding should fail */
    uint8_t hl = 4;
    forward_result_t r = forward_data(&rt, 0xCCCC, &hl, 2000);
    TEST_ASSERT_FALSE(r.should_send);
    TEST_ASSERT_TRUE(r.route_error);
}

void test_rerr_build_and_handle(void) {
    route_install(&rt, 0xCCCC, 0xBBBB, 2, 200, ROUTE_ACTIVE, 1000);
    bramble_rerr_t rerr = rerr_build(0xAAAA, 0xCCCC, 0xBBBB);
    TEST_ASSERT_EQUAL(PKT_TYPE_RERR, rerr.header.type);
    TEST_ASSERT_EQUAL(0xAAAA, rerr.reporter_addr);
    TEST_ASSERT_EQUAL(0xCCCC, rerr.broken_dest);
    TEST_ASSERT_EQUAL(0xBBBB, rerr.broken_next_hop);

    bool marked = rerr_handle(&rt, &rerr);
    TEST_ASSERT_TRUE(marked);
    route_entry_t* e = route_lookup(&rt, 0xCCCC);
    TEST_ASSERT_EQUAL(ROUTE_BROKEN, e->state);
}

void test_rerr_wrong_next_hop_ignored(void) {
    route_install(&rt, 0xCCCC, 0xBBBB, 2, 200, ROUTE_ACTIVE, 1000);
    bramble_rerr_t rerr = rerr_build(0xAAAA, 0xCCCC, 0x9999); /* wrong next_hop */
    bool marked = rerr_handle(&rt, &rerr);
    TEST_ASSERT_FALSE(marked);
    route_entry_t* e = route_lookup(&rt, 0xCCCC);
    TEST_ASSERT_EQUAL(ROUTE_ACTIVE, e->state); /* unchanged */
}

/* Task 2: mesh_task.c's handle_rerr used to bump fail_count itself right
 * alongside marking a route broken. That side effect now lives in
 * rerr_handle so firmware and gosim both get it instead of only whichever
 * hand-copy happened to remember it. */
void test_rerr_handle_bumps_fail_count(void) {
    route_install(&rt, 0xCCCC, 0xBBBB, 2, 200, ROUTE_ACTIVE, 1000);
    bramble_rerr_t rerr = rerr_build(0xAAAA, 0xCCCC, 0xBBBB);
    rerr_handle(&rt, &rerr);
    route_entry_t* e = route_lookup(&rt, 0xCCCC);
    TEST_ASSERT_EQUAL(1, e->fail_count);
}

/* Task 3 (Phase 1 delivery-core plan): data_rx_decide extracts the
 * deliver-locally-vs-forward fork out of mesh_task.c's mesh_process_rx_packet
 * PKT_TYPE_DATA case, behavior-preserving. Task 4 turns on reverse-route
 * learning (dest=src_addr, next_hop=prev_hop) for every case below; these
 * three now also assert the reverse-route fields land correctly alongside
 * the original deliver/forward action. */
#define SELF_ADDR 0xAAAAu
#define ORIGIN_ADDR 0x0A0A0A0Au
#define PREV_HOP_ADDR 0x0B0B0B0Bu
#define OTHER_UNICAST_DEST 0xCCCCu

void test_data_rx_decide_deliver_self(void) {
    data_rx_decision_t d =
        data_rx_decide(SELF_ADDR, SELF_ADDR, ORIGIN_ADDR, PREV_HOP_ADDR, ROUTE_HOP_LIMIT_MAX, 200);
    TEST_ASSERT_EQUAL(DATA_RX_DELIVER, d.action);
    TEST_ASSERT_TRUE(d.install_reverse_route);
    TEST_ASSERT_EQUAL_UINT32(ORIGIN_ADDR, d.reverse_dest);
    TEST_ASSERT_EQUAL_UINT32(PREV_HOP_ADDR, d.reverse_next_hop);
    TEST_ASSERT_EQUAL(1, d.reverse_hop_count);
    TEST_ASSERT_EQUAL(200, d.reverse_metric);
}

void test_data_rx_decide_deliver_broadcast(void) {
    data_rx_decision_t d =
        data_rx_decide(0xFFFFFFFF, SELF_ADDR, ORIGIN_ADDR, PREV_HOP_ADDR, ROUTE_HOP_LIMIT_MAX, 150);
    TEST_ASSERT_EQUAL(DATA_RX_DELIVER, d.action);
    /* Broadcast DATA's sender is just as reachable via prev_hop as a
     * unicast sender is; install_reverse_route does not depend on action. */
    TEST_ASSERT_TRUE(d.install_reverse_route);
    TEST_ASSERT_EQUAL_UINT32(ORIGIN_ADDR, d.reverse_dest);
    TEST_ASSERT_EQUAL_UINT32(PREV_HOP_ADDR, d.reverse_next_hop);
}

void test_data_rx_decide_forward_other_unicast(void) {
    data_rx_decision_t d = data_rx_decide(OTHER_UNICAST_DEST, SELF_ADDR, ORIGIN_ADDR, PREV_HOP_ADDR,
                                          ROUTE_HOP_LIMIT_MAX, 100);
    TEST_ASSERT_EQUAL(DATA_RX_FORWARD, d.action);
    TEST_ASSERT_TRUE(d.install_reverse_route);
    TEST_ASSERT_EQUAL_UINT32(ORIGIN_ADDR, d.reverse_dest);
    TEST_ASSERT_EQUAL_UINT32(PREV_HOP_ADDR, d.reverse_next_hop);
}

/* Skip conditions: never install a self-referential route. */
void test_data_rx_decide_skips_when_src_is_self(void) {
    data_rx_decision_t d = data_rx_decide(OTHER_UNICAST_DEST, SELF_ADDR, SELF_ADDR, PREV_HOP_ADDR,
                                          ROUTE_HOP_LIMIT_MAX, 100);
    TEST_ASSERT_FALSE(d.install_reverse_route);
}

void test_data_rx_decide_skips_when_prev_hop_is_self(void) {
    data_rx_decision_t d = data_rx_decide(OTHER_UNICAST_DEST, SELF_ADDR, ORIGIN_ADDR, SELF_ADDR,
                                          ROUTE_HOP_LIMIT_MAX, 100);
    TEST_ASSERT_FALSE(d.install_reverse_route);
}

/* Hop-count derivation, verified against the A-B-C scenario from the brief:
 * A originates at ROUTE_HOP_LIMIT_MAX (never decremented before B receives
 * it) -> B learns A at 1 hop. B decrements once before forwarding, so C
 * receives ROUTE_HOP_LIMIT_MAX - 1 -> C learns A at 2 hops. */
void test_data_rx_decide_hop_count_one_hop_from_origin(void) {
    data_rx_decision_t d = data_rx_decide(OTHER_UNICAST_DEST, SELF_ADDR, ORIGIN_ADDR, PREV_HOP_ADDR,
                                          ROUTE_HOP_LIMIT_MAX, 100);
    TEST_ASSERT_EQUAL(1, d.reverse_hop_count);
}

void test_data_rx_decide_hop_count_two_hops_from_origin(void) {
    data_rx_decision_t d = data_rx_decide(OTHER_UNICAST_DEST, SELF_ADDR, ORIGIN_ADDR, PREV_HOP_ADDR,
                                          ROUTE_HOP_LIMIT_MAX - 1, 100);
    TEST_ASSERT_EQUAL(2, d.reverse_hop_count);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_forward_active_route);
    RUN_TEST(test_forward_stale_promotion);
    RUN_TEST(test_forward_ttl_expiry);
    RUN_TEST(test_forward_unknown_dest);
    RUN_TEST(test_forward_broken_after_failures);
    RUN_TEST(test_rerr_build_and_handle);
    RUN_TEST(test_rerr_wrong_next_hop_ignored);
    RUN_TEST(test_rerr_handle_bumps_fail_count);
    RUN_TEST(test_data_rx_decide_deliver_self);
    RUN_TEST(test_data_rx_decide_deliver_broadcast);
    RUN_TEST(test_data_rx_decide_forward_other_unicast);
    RUN_TEST(test_data_rx_decide_skips_when_src_is_self);
    RUN_TEST(test_data_rx_decide_skips_when_prev_hop_is_self);
    RUN_TEST(test_data_rx_decide_hop_count_one_hop_from_origin);
    RUN_TEST(test_data_rx_decide_hop_count_two_hops_from_origin);
    return UNITY_END();
}
