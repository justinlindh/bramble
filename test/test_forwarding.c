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
 * PKT_TYPE_DATA case, behavior-preserving. */
void test_data_rx_decide_deliver_self(void) {
    data_rx_decision_t d = data_rx_decide(0xAAAA, 0xAAAA);
    TEST_ASSERT_EQUAL(DATA_RX_DELIVER, d.action);
    TEST_ASSERT_FALSE(d.install_reverse_route);
}

void test_data_rx_decide_deliver_broadcast(void) {
    data_rx_decision_t d = data_rx_decide(0xFFFFFFFF, 0xAAAA);
    TEST_ASSERT_EQUAL(DATA_RX_DELIVER, d.action);
    TEST_ASSERT_FALSE(d.install_reverse_route);
}

void test_data_rx_decide_forward_other_unicast(void) {
    data_rx_decision_t d = data_rx_decide(0xCCCC, 0xAAAA);
    TEST_ASSERT_EQUAL(DATA_RX_FORWARD, d.action);
    TEST_ASSERT_FALSE(d.install_reverse_route);
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
    return UNITY_END();
}
