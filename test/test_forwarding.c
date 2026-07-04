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

    rerr_handle(&rt, &rerr);
    route_entry_t* e = route_lookup(&rt, 0xCCCC);
    TEST_ASSERT_EQUAL(ROUTE_BROKEN, e->state);
}

void test_rerr_wrong_next_hop_ignored(void) {
    route_install(&rt, 0xCCCC, 0xBBBB, 2, 200, ROUTE_ACTIVE, 1000);
    bramble_rerr_t rerr = rerr_build(0xAAAA, 0xCCCC, 0x9999); /* wrong next_hop */
    rerr_handle(&rt, &rerr);
    route_entry_t* e = route_lookup(&rt, 0xCCCC);
    TEST_ASSERT_EQUAL(ROUTE_ACTIVE, e->state); /* unchanged */
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
    return UNITY_END();
}
