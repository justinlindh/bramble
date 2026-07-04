#include "unity.h"
#include "../components/routing/routing.c"

static routing_table_t rt;

void setUp(void) { route_init(&rt); }
void tearDown(void) {}

void test_route_init_empty(void) { TEST_ASSERT_EQUAL(0, route_count(&rt)); }

void test_route_install_and_lookup(void) {
    route_install(&rt, 0xDEAD, 0x0001, 3, 200, ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, 1000);
    route_entry_t* e = route_lookup(&rt, 0xDEAD);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(0x0001, e->next_hop);
    TEST_ASSERT_EQUAL(3, e->hop_count);
    TEST_ASSERT_EQUAL(200, e->metric);
    TEST_ASSERT_EQUAL(ROUTE_ACTIVE, e->state);
}

void test_route_update_better_metric(void) {
    route_install(&rt, 0xDEAD, 0x0001, 3, 100, ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, 1000);
    route_install(&rt, 0xDEAD, 0x0002, 2, 200, ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, 2000);
    TEST_ASSERT_EQUAL(1, route_count(&rt));
    route_entry_t* e = route_lookup(&rt, 0xDEAD);
    TEST_ASSERT_EQUAL(200, e->metric);
    TEST_ASSERT_EQUAL(0x0002, e->next_hop);
}

/* Task 4-fix F2 (High) attack regression: red-team unbeatable poison metric.
 * A nearby attacker forges a DATA breadcrumb with the maximal metric (255)
 * and a 1-hop count -- the strongest possible entry under the plain
 * metric/hop rule, which nothing could ever beat or reclaim. The trust class
 * must make a DISCOVERED (control-plane, HMAC-gated) install ALWAYS win over
 * that breadcrumb, regardless of metric or hop count, so the real route is
 * never locked out. */
void test_f2_discovered_reclaims_over_poison_breadcrumb(void) {
    /* Attacker's maxed breadcrumb: metric 255, 1 hop, via the attacker. */
    route_install(&rt, 0xDEAD, 0xA77ACC, 1, 255, ROUTE_ACTIVE, ROUTE_SRC_BREADCRUMB, 1000);
    route_entry_t* e = route_lookup(&rt, 0xDEAD);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(0xA77ACC, e->next_hop);

    /* A legitimate DISCOVERED route with a WORSE metric and MORE hops must
     * still reclaim the entry: trust class dominates metric arbitration. */
    route_install(&rt, 0xDEAD, 0x600D, 4, 10, ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, 2000);
    TEST_ASSERT_EQUAL(1, route_count(&rt));
    e = route_lookup(&rt, 0xDEAD);
    TEST_ASSERT_EQUAL_UINT32(0x600D, e->next_hop);
    TEST_ASSERT_EQUAL(ROUTE_SRC_DISCOVERED, e->source);
    TEST_ASSERT_EQUAL(10, e->metric);
}

/* Task 4-fix F2 dual invariant: a breadcrumb must NEVER displace an existing
 * DISCOVERED route, even with a maxed (255,1) metric. */
void test_f2_breadcrumb_cannot_displace_discovered(void) {
    route_install(&rt, 0xDEAD, 0x600D, 4, 10, ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, 1000);
    /* Attacker's strongest possible breadcrumb: rejected outright. */
    route_install(&rt, 0xDEAD, 0xA77ACC, 1, 255, ROUTE_ACTIVE, ROUTE_SRC_BREADCRUMB, 2000);
    TEST_ASSERT_EQUAL(1, route_count(&rt));
    route_entry_t* e = route_lookup(&rt, 0xDEAD);
    TEST_ASSERT_EQUAL_UINT32(0x600D, e->next_hop);
    TEST_ASSERT_EQUAL(ROUTE_SRC_DISCOVERED, e->source);
}

/* Breadcrumb-vs-breadcrumb keeps the normal metric/hop arbitration: a better
 * breadcrumb still refreshes a worse one (the legitimate reverse-route path
 * must still self-heal). */
void test_f2_breadcrumb_metric_rule_preserved(void) {
    route_install(&rt, 0xDEAD, 0x0001, 3, 100, ROUTE_ACTIVE, ROUTE_SRC_BREADCRUMB, 1000);
    route_install(&rt, 0xDEAD, 0x0002, 2, 200, ROUTE_ACTIVE, ROUTE_SRC_BREADCRUMB, 2000);
    route_entry_t* e = route_lookup(&rt, 0xDEAD);
    TEST_ASSERT_EQUAL_UINT32(0x0002, e->next_hop);
    TEST_ASSERT_EQUAL(200, e->metric);
}

void test_route_maintenance_active_to_stale(void) {
    route_install(&rt, 0xDEAD, 0x0001, 3, 200, ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, 1000);
    route_maintenance(&rt, 1000 + ROUTE_ACTIVE_TIMEOUT_MS);
    route_entry_t* e = route_lookup(&rt, 0xDEAD);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(ROUTE_STALE, e->state);
}

void test_route_maintenance_stale_removed(void) {
    route_install(&rt, 0xDEAD, 0x0001, 3, 200, ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, 1000);
    /* First make it stale */
    route_maintenance(&rt, 1000 + ROUTE_ACTIVE_TIMEOUT_MS);
    /* Then remove after STALE_TIMEOUT from last_confirmed */
    route_maintenance(&rt, 1000 + ROUTE_STALE_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(0, route_count(&rt));
}

void test_route_unverified_state(void) {
    route_install(&rt, 0xBEEF, 0x0001, 2, 150, ROUTE_UNVERIFIED, ROUTE_SRC_DISCOVERED, 1000);
    route_entry_t* e = route_lookup(&rt, 0xBEEF);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(ROUTE_UNVERIFIED, e->state);
}

void test_rreq_dedup(void) {
    rreq_dedup_t cache;
    rreq_dedup_init(&cache);
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&cache, 42, 1000));
    TEST_ASSERT_TRUE(rreq_dedup_check_and_add(&cache, 42, 1000));
    /* After expiry */
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&cache, 42, 1000 + RREQ_DEDUP_EXPIRY_MS + 1));
}

void test_reverse_route(void) {
    reverse_route_table_t rvt;
    reverse_route_init(&rvt);
    reverse_route_add(&rvt, 99, 0xAA, 1000);
    reverse_route_t* r = reverse_route_lookup(&rvt, 99);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(0xAA, r->prev_hop);
    /* Purge after expiry */
    reverse_route_purge(&rvt, 1000 + REVERSE_ROUTE_EXPIRY_MS + 1);
    TEST_ASSERT_NULL(reverse_route_lookup(&rvt, 99));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_route_init_empty);
    RUN_TEST(test_route_install_and_lookup);
    RUN_TEST(test_route_update_better_metric);
    RUN_TEST(test_f2_discovered_reclaims_over_poison_breadcrumb);
    RUN_TEST(test_f2_breadcrumb_cannot_displace_discovered);
    RUN_TEST(test_f2_breadcrumb_metric_rule_preserved);
    RUN_TEST(test_route_maintenance_active_to_stale);
    RUN_TEST(test_route_maintenance_stale_removed);
    RUN_TEST(test_route_unverified_state);
    RUN_TEST(test_rreq_dedup);
    RUN_TEST(test_reverse_route);
    return UNITY_END();
}
