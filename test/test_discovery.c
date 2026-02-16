#include "unity.h"
#include "../components/routing/discovery.c"
#include "../components/routing/routing.c"

static pending_discovery_table_t dtbl;
static routing_table_t rt_a, rt_b, rt_c;
static reverse_route_table_t rev_a, rev_b, rev_c;
static rreq_dedup_t dedup_b, dedup_c;

void setUp(void) {
    discovery_init(&dtbl);
    route_init(&rt_a); route_init(&rt_b); route_init(&rt_c);
    reverse_route_init(&rev_a); reverse_route_init(&rev_b); reverse_route_init(&rev_c);
    rreq_dedup_init(&dedup_b); rreq_dedup_init(&dedup_c);
}
void tearDown(void) {}

/* Addresses */
#define ADDR_A 0x0A0A0A0A
#define ADDR_B 0x0B0B0B0B
#define ADDR_C 0x0C0C0C0C
#define QUERY  0xDEAD0001

/* --- Pending discovery table tests --- */

void test_discovery_start_and_lookup(void) {
    TEST_ASSERT_EQUAL(0, discovery_start(&dtbl, ADDR_C, QUERY, 1000));
    pending_discovery_t *d = discovery_lookup(&dtbl, ADDR_C);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL(ADDR_C, d->dest_addr);
    TEST_ASSERT_EQUAL(QUERY, d->query_id);
    TEST_ASSERT_EQUAL(1, d->attempts);
}

void test_discovery_lookup_by_query(void) {
    discovery_start(&dtbl, ADDR_C, QUERY, 1000);
    pending_discovery_t *d = discovery_lookup_by_query(&dtbl, QUERY);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL(ADDR_C, d->dest_addr);
}

void test_discovery_remove(void) {
    discovery_start(&dtbl, ADDR_C, QUERY, 1000);
    discovery_remove(&dtbl, ADDR_C);
    TEST_ASSERT_NULL(discovery_lookup(&dtbl, ADDR_C));
    TEST_ASSERT_EQUAL(0, dtbl.count);
}

void test_discovery_table_full(void) {
    for (int i = 0; i < MAX_PENDING_DISCOVERIES; i++)
        discovery_start(&dtbl, 0x100 + i, 0x200 + i, 1000);
    TEST_ASSERT_EQUAL(-1, discovery_start(&dtbl, 0x999, 0x999, 1000));
}

void test_discovery_should_retry(void) {
    discovery_start(&dtbl, ADDR_C, QUERY, 1000);
    pending_discovery_t *d = discovery_lookup(&dtbl, ADDR_C);
    /* Too early */
    TEST_ASSERT_FALSE(discovery_should_retry(d, 3000));
    /* After first interval */
    TEST_ASSERT_TRUE(discovery_should_retry(d, 7000));
    /* Record attempt */
    discovery_record_attempt(d, 7000);
    TEST_ASSERT_EQUAL(2, d->attempts);
    /* Too early for second retry */
    TEST_ASSERT_FALSE(discovery_should_retry(d, 10000));
    /* After second interval (15s) */
    TEST_ASSERT_TRUE(discovery_should_retry(d, 23000));
    /* Record third attempt — no more retries */
    discovery_record_attempt(d, 23000);
    TEST_ASSERT_FALSE(discovery_should_retry(d, 100000));
}

/* --- RREQ/RREP building tests --- */

void test_rreq_build_originator(void) {
    bramble_rreq_t r = rreq_build_originator(ADDR_A, ADDR_C, QUERY, 0xEEEE);
    TEST_ASSERT_EQUAL(PKT_TYPE_RREQ, r.header.type);
    TEST_ASSERT_EQUAL(ADDR_C, r.header.dest_addr);
    TEST_ASSERT_EQUAL(0, r.hop_count);
    TEST_ASSERT_EQUAL(255, r.metric);
    TEST_ASSERT_EQUAL(4, r.header.hop_limit);
    TEST_ASSERT_EQUAL(ADDR_A, r.prev_hop);
    TEST_ASSERT_EQUAL(0xEEEE, r.encrypted_source);
}

void test_rreq_forward(void) {
    bramble_rreq_t orig = rreq_build_originator(ADDR_A, ADDR_C, QUERY, 0xEEEE);
    bramble_rreq_t fwd = rreq_forward(&orig, ADDR_B, -80, 5);
    TEST_ASSERT_EQUAL(1, fwd.hop_count);
    TEST_ASSERT_EQUAL(3, fwd.header.hop_limit);
    TEST_ASSERT_EQUAL(ADDR_B, fwd.prev_hop);
    TEST_ASSERT_TRUE(fwd.metric < 255); /* penalty applied */
}

void test_rrep_build_destination(void) {
    bramble_rreq_t rreq = rreq_build_originator(ADDR_A, ADDR_C, QUERY, 0xEEEE);
    bramble_rreq_t fwd = rreq_forward(&rreq, ADDR_B, -70, 8);
    bramble_rrep_t rrep = rrep_build_destination(&fwd, ADDR_C);
    TEST_ASSERT_EQUAL(PKT_TYPE_RREP, rrep.header.type);
    TEST_ASSERT_EQUAL(QUERY, rrep.query_id);
    TEST_ASSERT_EQUAL(ADDR_C, rrep.src_addr);
    TEST_ASSERT_EQUAL(2, rrep.hop_count); /* fwd.hop_count(1) + 1 */
    TEST_ASSERT_EQUAL(fwd.metric, rrep.route_metric);
    TEST_ASSERT_EQUAL(ADDR_B, rrep.next_hop);
}

void test_rrep_forward(void) {
    bramble_rrep_t rrep;
    memset(&rrep, 0, sizeof(rrep));
    rrep.next_hop = ADDR_B;
    bramble_rrep_t fwd = rrep_forward(&rrep, ADDR_A);
    TEST_ASSERT_EQUAL(ADDR_A, fwd.next_hop);
    TEST_ASSERT_EQUAL(ADDR_A, fwd.header.dest_addr);
}

/* --- Integration: 3-node route discovery A → B → C --- */

void test_three_node_discovery(void) {
    uint32_t now = 1000;

    /* Step 1: A starts discovery for C */
    discovery_start(&dtbl, ADDR_C, QUERY, now);
    bramble_rreq_t rreq_a = rreq_build_originator(ADDR_A, ADDR_C, QUERY, 0xEEEE);

    /* Step 2: B receives RREQ from A */
    /* B checks dedup */
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&dedup_b, rreq_a.query_id, now));
    /* B stores reverse route */
    reverse_route_add(&rev_b, rreq_a.query_id, rreq_a.prev_hop, now);
    /* B forwards RREQ */
    bramble_rreq_t rreq_b = rreq_forward(&rreq_a, ADDR_B, -75, 7);

    /* Step 3: C receives RREQ from B */
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&dedup_c, rreq_b.query_id, now));
    reverse_route_add(&rev_c, rreq_b.query_id, rreq_b.prev_hop, now);
    /* C is destination — build RREP */
    bramble_rrep_t rrep_c = rrep_build_destination(&rreq_b, ADDR_C);

    /* Step 4: B receives RREP */
    /* B installs forward route to C (direct neighbor) */
    route_install(&rt_b, ADDR_C, ADDR_C, rrep_c.hop_count, rrep_c.route_metric, ROUTE_ACTIVE, now);
    /* B looks up reverse route to forward RREP toward A */
    reverse_route_t *rev = reverse_route_lookup(&rev_b, rrep_c.query_id);
    TEST_ASSERT_NOT_NULL(rev);
    TEST_ASSERT_EQUAL(ADDR_A, rev->prev_hop);
    bramble_rrep_t rrep_b = rrep_forward(&rrep_c, rev->prev_hop);

    /* Step 5: A receives RREP */
    route_install(&rt_a, ADDR_C, ADDR_B, rrep_b.hop_count, rrep_b.route_metric, ROUTE_ACTIVE, now);
    discovery_remove(&dtbl, ADDR_C);

    /* Step 6: Verify */
    route_entry_t *ra = route_lookup(&rt_a, ADDR_C);
    TEST_ASSERT_NOT_NULL(ra);
    TEST_ASSERT_EQUAL(ADDR_B, ra->next_hop);
    TEST_ASSERT_EQUAL(ROUTE_ACTIVE, ra->state);

    route_entry_t *rb = route_lookup(&rt_b, ADDR_C);
    TEST_ASSERT_NOT_NULL(rb);
    TEST_ASSERT_EQUAL(ADDR_C, rb->next_hop); /* direct */

    /* Reverse routes exist */
    TEST_ASSERT_NOT_NULL(reverse_route_lookup(&rev_b, QUERY));
    TEST_ASSERT_NOT_NULL(reverse_route_lookup(&rev_c, QUERY));

    /* Discovery removed */
    TEST_ASSERT_NULL(discovery_lookup(&dtbl, ADDR_C));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_discovery_start_and_lookup);
    RUN_TEST(test_discovery_lookup_by_query);
    RUN_TEST(test_discovery_remove);
    RUN_TEST(test_discovery_table_full);
    RUN_TEST(test_discovery_should_retry);
    RUN_TEST(test_rreq_build_originator);
    RUN_TEST(test_rreq_forward);
    RUN_TEST(test_rrep_build_destination);
    RUN_TEST(test_rrep_forward);
    RUN_TEST(test_three_node_discovery);
    return UNITY_END();
}
