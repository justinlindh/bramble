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

/* Finding 2 (final whole-branch review): capacity-eviction victim selection
 * must never pick a DISCOVERED route as the eviction victim while a
 * BREADCRUMB victim exists anywhere in the table, and must not sacrifice a
 * DISCOVERED route for a BREADCRUMB install even when no BREADCRUMB victim
 * exists -- the same trust-class rule the same-dest F2 tests above already
 * enforce, extended to the MAX_ROUTES-full capacity path. */

/* Table full of nothing but DISCOVERED routes: a new BREADCRUMB install has
 * no lower-trust victim to reclaim, so the invariant "a breadcrumb install
 * never evicts a discovered route while any breadcrumb victim exists" can
 * only be honored by refusing the install outright -- evicting ANY of these
 * DISCOVERED entries to make room for a breadcrumb would be the exact
 * trust-class inversion F2 forbids for the same-dest case. */
void test_f2_capacity_eviction_refuses_breadcrumb_when_all_discovered(void) {
    for (int i = 0; i < MAX_ROUTES; i++) {
        route_install(&rt, 0x1000 + (uint32_t)i, 0x0001, 3, 200, ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED,
                      1000 + (uint32_t)i);
    }
    TEST_ASSERT_EQUAL(MAX_ROUTES, route_count(&rt));

    int ret =
        route_install(&rt, 0xF00D, 0xA77ACC, 1, 255, ROUTE_ACTIVE, ROUTE_SRC_BREADCRUMB, 999999);

    /* Refused: table size unchanged, the new dest was not installed, and
     * every original DISCOVERED entry is still present untouched. */
    TEST_ASSERT_EQUAL(-1, ret);
    TEST_ASSERT_EQUAL(MAX_ROUTES, route_count(&rt));
    TEST_ASSERT_NULL(route_lookup(&rt, 0xF00D));
    for (int i = 0; i < MAX_ROUTES; i++) {
        route_entry_t* e = route_lookup(&rt, 0x1000 + (uint32_t)i);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL(ROUTE_SRC_DISCOVERED, e->source);
    }
}

/* Table full but with one BREADCRUMB entry among many DISCOVERED ones: the
 * breadcrumb must be the victim, never a discovered route, even though the
 * breadcrumb is not the oldest (LRU) entry overall. */
void test_f2_capacity_eviction_prefers_breadcrumb_victim(void) {
    route_install(&rt, 0x2000, 0x0001, 3, 200, ROUTE_ACTIVE, ROUTE_SRC_BREADCRUMB, 5000);
    for (int i = 1; i < MAX_ROUTES; i++) {
        /* Older last_used than the breadcrumb above, so a source-blind LRU
         * scan would pick one of these DISCOVERED entries instead. */
        route_install(&rt, 0x1000 + (uint32_t)i, 0x0001, 3, 200, ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED,
                      1000 + (uint32_t)i);
    }
    TEST_ASSERT_EQUAL(MAX_ROUTES, route_count(&rt));

    int ret = route_install(&rt, 0xF00D, 0x600D, 4, 10, ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, 999999);

    TEST_ASSERT_TRUE(ret >= 0);
    TEST_ASSERT_EQUAL(MAX_ROUTES, route_count(&rt));
    TEST_ASSERT_NOT_NULL(route_lookup(&rt, 0xF00D));
    /* The lone breadcrumb is gone; every original DISCOVERED entry
     * survived. */
    TEST_ASSERT_NULL(route_lookup(&rt, 0x2000));
    for (int i = 1; i < MAX_ROUTES; i++) {
        route_entry_t* e = route_lookup(&rt, 0x1000 + (uint32_t)i);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL(ROUTE_SRC_DISCOVERED, e->source);
    }
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
    RUN_TEST(test_f2_capacity_eviction_refuses_breadcrumb_when_all_discovered);
    RUN_TEST(test_f2_capacity_eviction_prefers_breadcrumb_victim);
    RUN_TEST(test_route_maintenance_active_to_stale);
    RUN_TEST(test_route_maintenance_stale_removed);
    RUN_TEST(test_route_unverified_state);
    RUN_TEST(test_rreq_dedup);
    RUN_TEST(test_reverse_route);
    return UNITY_END();
}
