#include "unity.h"
#include "../components/routing/discovery.c"
#include "../components/routing/routing.c"
#include "test_net_key.h"

static pending_discovery_table_t dtbl;
static routing_table_t rt_a, rt_b, rt_c;
static reverse_route_table_t rev_a, rev_b, rev_c;
static rreq_dedup_t dedup_b, dedup_c;

void setUp(void) {
    /* Mandatory-provisioning (Task 2): RREP sign/verify in the discovery e2e
     * flows need the network key; provision the shared fixed key so a
     * provisioned mesh discovers routes exactly as before. */
    bramble_test_provision_net_key();
    discovery_init(&dtbl);
    route_init(&rt_a);
    route_init(&rt_b);
    route_init(&rt_c);
    reverse_route_init(&rev_a);
    reverse_route_init(&rev_b);
    reverse_route_init(&rev_c);
    rreq_dedup_init(&dedup_b);
    rreq_dedup_init(&dedup_c);
}
void tearDown(void) {}

/* Addresses */
#define ADDR_A 0x0A0A0A0A
#define ADDR_B 0x0B0B0B0B
#define ADDR_C 0x0C0C0C0C
#define ADDR_D 0x0D0D0D0D
#define QUERY 0xDEAD0001
#define QUERY2 0xDEAD0002
#define QUERY3 0xDEAD0003

/* --- Pending discovery table tests --- */

void test_discovery_start_and_lookup(void) {
    TEST_ASSERT_EQUAL(0, discovery_start(&dtbl, ADDR_C, QUERY, 1000));
    pending_discovery_t* d = discovery_lookup(&dtbl, ADDR_C);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL(ADDR_C, d->dest_addr);
    TEST_ASSERT_EQUAL(QUERY, discovery_current_query_id(d));
    TEST_ASSERT_EQUAL(1, d->attempts);
}

void test_discovery_lookup_by_query(void) {
    discovery_start(&dtbl, ADDR_C, QUERY, 1000);
    pending_discovery_t* d = discovery_lookup_by_query(&dtbl, QUERY);
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
    pending_discovery_t* d = discovery_lookup(&dtbl, ADDR_C);
    /* Too early */
    TEST_ASSERT_FALSE(discovery_should_retry(d, 3000));
    /* After first interval */
    TEST_ASSERT_TRUE(discovery_should_retry(d, 7000));
    /* Record attempt */
    discovery_record_attempt(d, QUERY2, 7000);
    TEST_ASSERT_EQUAL(2, d->attempts);
    /* Too early for second retry */
    TEST_ASSERT_FALSE(discovery_should_retry(d, 10000));
    /* After second interval (15s) */
    TEST_ASSERT_TRUE(discovery_should_retry(d, 23000));
    /* Record third attempt: no more retries after that */
    discovery_record_attempt(d, QUERY3, 23000);
    TEST_ASSERT_FALSE(discovery_should_retry(d, 100000));
}

/* --- Fresh query_id per retry (DES-2) --- */

void test_retry_uses_fresh_query_id(void) {
    discovery_start(&dtbl, ADDR_C, QUERY, 1000);
    pending_discovery_t* d = discovery_lookup(&dtbl, ADDR_C);
    TEST_ASSERT_EQUAL(QUERY, discovery_current_query_id(d));

    discovery_record_attempt(d, QUERY2, 6000);
    TEST_ASSERT_EQUAL(QUERY2, discovery_current_query_id(d));

    discovery_record_attempt(d, QUERY3, 21000);
    TEST_ASSERT_EQUAL(QUERY3, discovery_current_query_id(d));
}

void test_rrep_for_any_outstanding_attempt_matches(void) {
    discovery_start(&dtbl, ADDR_C, QUERY, 1000);
    pending_discovery_t* d = discovery_lookup(&dtbl, ADDR_C);
    discovery_record_attempt(d, QUERY2, 6000);
    discovery_record_attempt(d, QUERY3, 21000);

    /* An RREP answering ANY attempt's query_id completes the discovery. */
    TEST_ASSERT_EQUAL_PTR(d, discovery_lookup_by_query(&dtbl, QUERY));
    TEST_ASSERT_EQUAL_PTR(d, discovery_lookup_by_query(&dtbl, QUERY2));
    TEST_ASSERT_EQUAL_PTR(d, discovery_lookup_by_query(&dtbl, QUERY3));
    TEST_ASSERT_NULL(discovery_lookup_by_query(&dtbl, 0xBADBAD));
}

/* The DES-2 failure mode: a relay that heard attempt 1 silently ate the
 * same-query retries because the 5s/15s retry schedule sits inside the 30s
 * dedup window. Fresh query_ids must pass the relay's dedup. */
void test_retry_passes_relay_dedup_within_window(void) {
    uint32_t now = 1000;

    /* Attempt 1 reaches relay B; B forwards it but the flood dies beyond B. */
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&dedup_b, QUERY, now));

    /* Old behavior: retry at +5s with the SAME query_id is dropped by B. */
    TEST_ASSERT_TRUE(rreq_dedup_check_and_add(&dedup_b, QUERY, now + 5000));

    /* New behavior: retry carries a fresh query_id and B forwards it. */
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&dedup_b, QUERY2, now + 5000));

    /* Second retry at +20s, still inside the 30s window, also passes. */
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&dedup_b, QUERY3, now + 20000));
}

/* --- Expanding ring (DES-1) --- */

void test_hop_limit_expands_on_retry(void) {
    TEST_ASSERT_EQUAL(RREQ_HOP_LIMIT_INITIAL, discovery_hop_limit_for_attempt(1));
    TEST_ASSERT_EQUAL(RREQ_HOP_LIMIT_EXPANDED, discovery_hop_limit_for_attempt(2));
    TEST_ASSERT_EQUAL(RREQ_HOP_LIMIT_EXPANDED, discovery_hop_limit_for_attempt(3));
    TEST_ASSERT_EQUAL(4, RREQ_HOP_LIMIT_INITIAL);
    TEST_ASSERT_EQUAL(8, RREQ_HOP_LIMIT_EXPANDED);
}

/* --- RREQ forward jitter (DES-3) --- */

void test_forward_jitter_bounds(void) {
    TEST_ASSERT_EQUAL(RREQ_FWD_JITTER_MIN_MS, discovery_forward_jitter_ms(0));
    uint32_t span = RREQ_FWD_JITTER_MAX_MS - RREQ_FWD_JITTER_MIN_MS;
    TEST_ASSERT_EQUAL(RREQ_FWD_JITTER_MAX_MS, discovery_forward_jitter_ms(span));
    TEST_ASSERT_EQUAL(RREQ_FWD_JITTER_MIN_MS, discovery_forward_jitter_ms(span + 1));
    for (uint32_t r = 0; r < 2000; r += 13) {
        uint32_t j = discovery_forward_jitter_ms(r);
        TEST_ASSERT_TRUE(j >= RREQ_FWD_JITTER_MIN_MS && j <= RREQ_FWD_JITTER_MAX_MS);
    }
}

/* --- Link-metric helper (route metric bookkeeping) --- */

void test_dedup_expires_with_window(void) {
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&dedup_c, QUERY, 1000));
    /* Past the 30s window the query is forgotten entirely. */
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&dedup_c, QUERY, 1000 + RREQ_DEDUP_EXPIRY_MS));
}

void test_metric_apply_link_penalty_floors_at_zero(void) {
    /* Perfect link: no penalty */
    TEST_ASSERT_EQUAL(200, metric_apply_link_penalty(200, -50, 12));
    /* Terrible link: full penalty, floored at zero for tiny metrics */
    TEST_ASSERT_EQUAL(0, metric_apply_link_penalty(10, -125, -8));
    /* Penalty subtracts, never adds */
    TEST_ASSERT_TRUE(metric_apply_link_penalty(200, -100, 0) < 200);
}

/* --- RREQ/RREP building tests --- */

void test_rreq_build_originator(void) {
    bramble_rreq_t r =
        rreq_build_originator(ADDR_A, ADDR_C, QUERY, 0xEEEE, discovery_hop_limit_for_attempt(1));
    TEST_ASSERT_EQUAL(PKT_TYPE_RREQ, r.header.type);
    TEST_ASSERT_EQUAL(ADDR_C, r.header.dest_addr);
    TEST_ASSERT_EQUAL(0, r.hop_count);
    TEST_ASSERT_EQUAL(255, r.metric);
    TEST_ASSERT_EQUAL(4, r.header.hop_limit);
    TEST_ASSERT_EQUAL(ADDR_A, r.prev_hop);
    TEST_ASSERT_EQUAL(0xEEEE, r.encrypted_source);
}

void test_rreq_build_originator_expanded_ring(void) {
    bramble_rreq_t r =
        rreq_build_originator(ADDR_A, ADDR_C, QUERY2, 0xEEEE, discovery_hop_limit_for_attempt(2));
    TEST_ASSERT_EQUAL(8, r.header.hop_limit);
}

void test_rreq_forward(void) {
    bramble_rreq_t orig = rreq_build_originator(ADDR_A, ADDR_C, QUERY, 0xEEEE, 4);
    bramble_rreq_t fwd = rreq_forward(&orig, ADDR_B, -80, 5);
    TEST_ASSERT_EQUAL(1, fwd.hop_count);
    TEST_ASSERT_EQUAL(3, fwd.header.hop_limit);
    TEST_ASSERT_EQUAL(ADDR_B, fwd.prev_hop);
    TEST_ASSERT_TRUE(fwd.metric < 255); /* penalty applied */
}

/* Issue #74: hop_count is a single attacker-controlled wire byte on an
 * unauthenticated RREQ. A plain ++ / + wraps 255 -> 0, laundering a forged
 * maxed hop_count into a zero-hop (maximally attractive) advertisement.
 * rreq_forward and rrep_build_destination must saturate at 255 instead of
 * wrapping (rrep_build_intermediate's saturation is checked below, next to the
 * fresh_discovered_route helper it needs). */
void test_rreq_forward_hop_count_saturates(void) {
    bramble_rreq_t r = rreq_build_originator(ADDR_A, ADDR_C, QUERY, 0xEEEE, 8);
    r.hop_count = 255;
    bramble_rreq_t fwd = rreq_forward(&r, ADDR_B, -70, 8);
    TEST_ASSERT_EQUAL(255, fwd.hop_count); /* not 0 */
}

void test_rrep_build_destination_hop_count_saturates(void) {
    bramble_rreq_t r = rreq_build_originator(ADDR_A, ADDR_C, QUERY, 0xEEEE, 8);
    r.hop_count = 255;
    bramble_rrep_t rrep = rrep_build_destination(&r, ADDR_C);
    TEST_ASSERT_EQUAL(255, rrep.hop_count); /* not 0 */
}

void test_rrep_build_destination(void) {
    bramble_rreq_t rreq = rreq_build_originator(ADDR_A, ADDR_C, QUERY, 0xEEEE, 4);
    bramble_rreq_t fwd = rreq_forward(&rreq, ADDR_B, -70, 8);
    bramble_rrep_t rrep = rrep_build_destination(&fwd, ADDR_C);
    TEST_ASSERT_EQUAL(PKT_TYPE_RREP, rrep.header.type);
    TEST_ASSERT_EQUAL(QUERY, rrep.query_id);
    TEST_ASSERT_EQUAL(ADDR_C, rrep.src_addr);
    TEST_ASSERT_EQUAL(2, rrep.hop_count); /* fwd.hop_count(1) + 1 */
    TEST_ASSERT_EQUAL(fwd.metric, rrep.route_metric);
    /* next_hop is the destination's OWN address: it is the first hop toward
     * itself. header.dest_addr (unchanged) still carries the frame-routing
     * target (fwd.prev_hop == ADDR_B). */
    TEST_ASSERT_EQUAL(ADDR_C, rrep.next_hop);
    TEST_ASSERT_EQUAL(ADDR_B, rrep.header.dest_addr);
    /* RREP must be able to return along an expanded-ring path */
    TEST_ASSERT_EQUAL(ROUTE_HOP_LIMIT_MAX, rrep.header.hop_limit);
}

void test_rrep_forward(void) {
    bramble_rrep_t rrep;
    memset(&rrep, 0, sizeof(rrep));
    rrep.next_hop = ADDR_C; /* whatever the incoming next_hop was, gets overwritten */
    bramble_rrep_t fwd = rrep_forward(&rrep, ADDR_A, ADDR_B);
    TEST_ASSERT_EQUAL(ADDR_B, fwd.next_hop);         /* the forwarder's own address */
    TEST_ASSERT_EQUAL(ADDR_A, fwd.header.dest_addr); /* frame routing target, unchanged role */
}

/* --- rrep_rx_decide: behavior-preserving extraction of handle_rrep --- */

void test_rrep_rx_decide_originator_delivers(void) {
    bramble_rrep_t rrep;
    memset(&rrep, 0, sizeof(rrep));
    rrep.header.dest_addr = ADDR_A;
    rrep.query_id = QUERY;
    rrep.src_addr = ADDR_C;
    rrep.next_hop = ADDR_B; /* the forwarder that delivered this RREP to us */
    rrep.hop_count = 3;

    discovery_start(&dtbl, ADDR_C, QUERY, 1000);

    rrep_rx_decision_t d = rrep_rx_decide(&rrep, 210, &dtbl, &rev_a);

    TEST_ASSERT_EQUAL(RREP_RX_DELIVER, d.action);
    TEST_ASSERT_EQUAL(ADDR_C, d.deliver_dest);
    TEST_ASSERT_TRUE(d.install_route);
    TEST_ASSERT_EQUAL(ADDR_C, d.route_dest);
    TEST_ASSERT_EQUAL(ADDR_B, d.route_next_hop); /* rrep.next_hop: the forwarder, not src_addr */
    TEST_ASSERT_EQUAL(3, d.route_hops);
    TEST_ASSERT_EQUAL(210, d.route_metric);
}

void test_rrep_rx_decide_intermediate_forwards(void) {
    bramble_rrep_t rrep;
    memset(&rrep, 0, sizeof(rrep));
    rrep.header.dest_addr = ADDR_A; /* framed toward the originator, not us */
    rrep.query_id = QUERY;
    rrep.src_addr = ADDR_C;
    rrep.next_hop = ADDR_D; /* the forwarder that delivered this RREP to us */
    rrep.hop_count = 2;

    reverse_route_add(&rev_b, QUERY, ADDR_A, 1000);

    rrep_rx_decision_t d = rrep_rx_decide(&rrep, 180, &dtbl, &rev_b);

    TEST_ASSERT_EQUAL(RREP_RX_FORWARD, d.action);
    TEST_ASSERT_EQUAL(ADDR_A, d.forward_to);
    TEST_ASSERT_TRUE(d.install_route);
    TEST_ASSERT_EQUAL(ADDR_C, d.route_dest);
    TEST_ASSERT_EQUAL(ADDR_D, d.route_next_hop); /* rrep.next_hop directly */
    TEST_ASSERT_EQUAL(2, d.route_hops);
    TEST_ASSERT_EQUAL(180, d.route_metric);
}

void test_rrep_rx_decide_unsolicited_drops_without_install(void) {
    bramble_rrep_t rrep;
    memset(&rrep, 0, sizeof(rrep));
    rrep.header.dest_addr = ADDR_A;
    rrep.query_id = QUERY;
    rrep.src_addr = ADDR_C;
    rrep.next_hop = ADDR_D;
    rrep.hop_count = 1;

    /* No pd, no reverse route for this query: we never participated in this
     * discovery, so the participation gate drops before install. */
    rrep_rx_decision_t d = rrep_rx_decide(&rrep, 150, &dtbl, &rev_b);

    TEST_ASSERT_EQUAL(RREP_RX_DROP, d.action);
    TEST_ASSERT_FALSE(d.install_route);
    TEST_ASSERT_EQUAL(ADDR_C, d.route_dest);
    TEST_ASSERT_EQUAL(ADDR_D, d.route_next_hop);
    TEST_ASSERT_EQUAL(1, d.route_hops);
    TEST_ASSERT_EQUAL(150, d.route_metric);
}

/* --- Integration: 3-node route discovery A -> B -> C --- */

void test_three_node_discovery(void) {
    uint32_t now = 1000;

    /* Step 1: A starts discovery for C */
    discovery_start(&dtbl, ADDR_C, QUERY, now);
    bramble_rreq_t rreq_a =
        rreq_build_originator(ADDR_A, ADDR_C, QUERY, 0xEEEE, discovery_hop_limit_for_attempt(1));

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
    /* C is the destination: build RREP */
    bramble_rrep_t rrep_c = rrep_build_destination(&rreq_b, ADDR_C);

    /* Step 4: B receives RREP */
    /* B installs forward route to C (direct neighbor) */
    route_install(&rt_b, ADDR_C, ADDR_C, rrep_c.hop_count, rrep_c.route_metric, ROUTE_ACTIVE,
                  ROUTE_SRC_DISCOVERED, now);
    /* B looks up reverse route to forward RREP toward A */
    reverse_route_t* rev = reverse_route_lookup(&rev_b, rrep_c.query_id);
    TEST_ASSERT_NOT_NULL(rev);
    TEST_ASSERT_EQUAL(ADDR_A, rev->prev_hop);
    bramble_rrep_t rrep_b = rrep_forward(&rrep_c, rev->prev_hop, ADDR_B);

    /* Step 5: A receives RREP */
    route_install(&rt_a, ADDR_C, ADDR_B, rrep_b.hop_count, rrep_b.route_metric, ROUTE_ACTIVE,
                  ROUTE_SRC_DISCOVERED, now);
    discovery_remove(&dtbl, ADDR_C);

    /* Step 6: Verify */
    route_entry_t* ra = route_lookup(&rt_a, ADDR_C);
    TEST_ASSERT_NOT_NULL(ra);
    TEST_ASSERT_EQUAL(ADDR_B, ra->next_hop);
    TEST_ASSERT_EQUAL(ROUTE_ACTIVE, ra->state);

    route_entry_t* rb = route_lookup(&rt_b, ADDR_C);
    TEST_ASSERT_NOT_NULL(rb);
    TEST_ASSERT_EQUAL(ADDR_C, rb->next_hop); /* direct */

    /* Reverse routes exist */
    TEST_ASSERT_NOT_NULL(reverse_route_lookup(&rev_b, QUERY));
    TEST_ASSERT_NOT_NULL(reverse_route_lookup(&rev_c, QUERY));

    /* Discovery removed */
    TEST_ASSERT_NULL(discovery_lookup(&dtbl, ADDR_C));
}

/* --- Integration: retry after the flood died, relay dedup still warm --- */

void test_retry_discovery_succeeds_through_warm_dedup(void) {
    uint32_t now = 1000;

    /* Attempt 1: A floods QUERY. B forwards it, but C never hears it
     * (collision). B's dedup is now warm for QUERY. */
    discovery_start(&dtbl, ADDR_C, QUERY, now);
    pending_discovery_t* d = discovery_lookup(&dtbl, ADDR_C);
    bramble_rreq_t rreq1 = rreq_build_originator(ADDR_A, ADDR_C, QUERY, 0xEEE1,
                                                 discovery_hop_limit_for_attempt(d->attempts));
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&dedup_b, rreq1.query_id, now));
    reverse_route_add(&rev_b, rreq1.query_id, rreq1.prev_hop, now);

    /* Attempt 2 at +5s: fresh query_id. B must forward it (old code: eaten). */
    now += RREQ_RETRY_INTERVAL_1_MS;
    TEST_ASSERT_TRUE(discovery_should_retry(d, now));
    discovery_record_attempt(d, QUERY2, now);
    bramble_rreq_t rreq2 = rreq_build_originator(ADDR_A, ADDR_C, QUERY2, 0xEEE2,
                                                 discovery_hop_limit_for_attempt(d->attempts));
    TEST_ASSERT_EQUAL(RREQ_HOP_LIMIT_EXPANDED, rreq2.header.hop_limit);
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&dedup_b, rreq2.query_id, now));
    reverse_route_add(&rev_b, rreq2.query_id, rreq2.prev_hop, now);
    bramble_rreq_t rreq2_fwd = rreq_forward(&rreq2, ADDR_B, -75, 7);

    /* C hears the retry this time and answers. */
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&dedup_c, rreq2_fwd.query_id, now));
    reverse_route_add(&rev_c, rreq2_fwd.query_id, rreq2_fwd.prev_hop, now);
    bramble_rrep_t rrep = rrep_build_destination(&rreq2_fwd, ADDR_C);

    /* The RREP answers attempt 2; A's pending discovery must match it. */
    TEST_ASSERT_EQUAL_PTR(d, discovery_lookup_by_query(&dtbl, rrep.query_id));
    /* ...and would also have matched a late answer to attempt 1. */
    TEST_ASSERT_EQUAL_PTR(d, discovery_lookup_by_query(&dtbl, QUERY));
}

/* --- Phase 2 "save reactive routing" Part B: intermediate-node RREP --- */

static route_entry_t fresh_discovered_route(uint32_t dest, uint32_t next_hop, uint8_t hop_count,
                                            uint8_t metric, uint32_t now_ms) {
    route_entry_t r;
    memset(&r, 0, sizeof(r));
    r.dest_addr = dest;
    r.next_hop = next_hop;
    r.hop_count = hop_count;
    r.metric = metric;
    r.state = ROUTE_ACTIVE;
    r.source = ROUTE_SRC_DISCOVERED;
    r.last_confirmed = now_ms;
    r.last_used = now_ms;
    return r;
}

void test_intermediate_rrep_route_usable_accepts_fresh_discovered_active(void) {
    route_entry_t r = fresh_discovered_route(ADDR_D, ADDR_C, 1, 200, 10000);
    TEST_ASSERT_TRUE(intermediate_rrep_route_usable(&r, 10000));
    /* Still within the freshness window */
    TEST_ASSERT_TRUE(intermediate_rrep_route_usable(&r, 10000 + INTERMEDIATE_RREP_MAX_AGE_MS));
}

void test_intermediate_rrep_route_usable_rejects_null(void) {
    TEST_ASSERT_FALSE(intermediate_rrep_route_usable(NULL, 10000));
}

void test_intermediate_rrep_route_usable_rejects_breadcrumb(void) {
    /* A DATA-forwarding breadcrumb: unauthenticated next-hop hint (see
     * route_source_t doc comment). Must never be used to author a reply on
     * someone else's behalf. */
    route_entry_t r = fresh_discovered_route(ADDR_D, ADDR_C, 1, 255, 10000);
    r.source = ROUTE_SRC_BREADCRUMB;
    TEST_ASSERT_FALSE(intermediate_rrep_route_usable(&r, 10000));
}

void test_intermediate_rrep_route_usable_rejects_stale_and_broken(void) {
    route_entry_t stale = fresh_discovered_route(ADDR_D, ADDR_C, 1, 200, 10000);
    stale.state = ROUTE_STALE;
    TEST_ASSERT_FALSE(intermediate_rrep_route_usable(&stale, 10000));

    route_entry_t broken = fresh_discovered_route(ADDR_D, ADDR_C, 1, 200, 10000);
    broken.state = ROUTE_BROKEN;
    TEST_ASSERT_FALSE(intermediate_rrep_route_usable(&broken, 10000));
}

void test_intermediate_rrep_route_usable_rejects_too_old(void) {
    /* ACTIVE state alone is too coarse a freshness signal (no destination
     * sequence numbers exist to check instead): last_confirmed beyond the
     * tight INTERMEDIATE_RREP_MAX_AGE_MS window must still be rejected even
     * though the route has not yet transitioned out of ROUTE_ACTIVE
     * (that transition only happens at the much longer
     * ROUTE_ACTIVE_TIMEOUT_MS). */
    route_entry_t r = fresh_discovered_route(ADDR_D, ADDR_C, 1, 200, 10000);
    uint32_t just_too_old = 10000 + INTERMEDIATE_RREP_MAX_AGE_MS + 1;
    TEST_ASSERT_TRUE(just_too_old < ROUTE_ACTIVE_TIMEOUT_MS + 10000); /* still ACTIVE by state */
    TEST_ASSERT_FALSE(intermediate_rrep_route_usable(&r, just_too_old));
}

void test_rrep_build_intermediate_hop_and_metric_math(void) {
    /* Originator A -> relay B -> intermediate I: the RREQ I (ADDR_C, reused
     * as "I" here) receives has already been forwarded once by B, so
     * rreq.hop_count == 1 (hops from A to B) and rreq.prev_hop == B. */
    bramble_rreq_t rreq = rreq_build_originator(ADDR_A, ADDR_D, QUERY, 0xEEEE, 8);
    bramble_rreq_t at_i = rreq_forward(&rreq, ADDR_B, -70, 8);
    TEST_ASSERT_EQUAL(1, at_i.hop_count);
    TEST_ASSERT_EQUAL(ADDR_B, at_i.prev_hop);

    /* I's cached route to D: 2 hops away, metric 180. */
    route_entry_t route_to_d =
        fresh_discovered_route(ADDR_D, ADDR_B /* unused here */, 2, 180, 5000);

    bramble_rrep_t rrep = rrep_build_intermediate(&at_i, &route_to_d, ADDR_C, -70, 8);

    TEST_ASSERT_EQUAL(PKT_TYPE_RREP, rrep.header.type);
    TEST_ASSERT_EQUAL(QUERY, rrep.query_id);
    /* Answering ON BEHALF OF D, not as I. */
    TEST_ASSERT_EQUAL(ADDR_D, rrep.src_addr);
    /* I is the first hop back, from its own perspective. */
    TEST_ASSERT_EQUAL(ADDR_C, rrep.next_hop);
    /* Frame routes back toward whoever sent I this RREQ. */
    TEST_ASSERT_EQUAL(ADDR_B, rrep.header.dest_addr);
    /* hop_count = (rreq.hop_count(1) + 1) + route.hop_count(2) = 4: total
     * accumulated path length A -> B -> I -> ... -> D. */
    TEST_ASSERT_EQUAL(4, rrep.hop_count);
    TEST_ASSERT_EQUAL(ROUTE_HOP_LIMIT_MAX, rrep.header.hop_limit);
    /* Metric composition: metric_to_me = at_i.metric link-penalized once
     * more for the B->I hop, then the route's own accumulated penalty
     * (255 - route_to_d.metric) subtracts further, floored at zero. */
    uint8_t metric_to_me = metric_apply_link_penalty(at_i.metric, -70, 8);
    uint16_t dest_penalty = (uint16_t)(255 - route_to_d.metric);
    uint8_t expect_metric =
        (dest_penalty >= metric_to_me) ? 0 : (uint8_t)(metric_to_me - dest_penalty);
    TEST_ASSERT_EQUAL(expect_metric, rrep.route_metric);

    /* Authenticated exactly like any other RREP: a receiver's rrep_verify
     * must accept it. */
    TEST_ASSERT_TRUE(rrep_verify(&rrep));
}

/* Issue #74 (intermediate branch): a forged maxed rreq.hop_count must not wrap
 * through rrep_build_intermediate's (hop+1)+route_hops arithmetic into a tiny
 * total that advertises a bogus short path on the destination's behalf. */
void test_rrep_build_intermediate_hop_count_saturates(void) {
    bramble_rreq_t rreq = rreq_build_originator(ADDR_A, ADDR_D, QUERY, 0xEEEE, 8);
    rreq.hop_count = 255;
    route_entry_t route_to_d = fresh_discovered_route(ADDR_D, ADDR_B, 5, 180, 5000);
    bramble_rrep_t rrep = rrep_build_intermediate(&rreq, &route_to_d, ADDR_C, -70, 8);
    /* (255 + 1 -> 255) + 5 -> 255, never wraps to a tiny forged total. */
    TEST_ASSERT_EQUAL(255, rrep.hop_count);
}

void test_rrep_build_intermediate_tamper_fails_verify(void) {
    bramble_rreq_t rreq = rreq_build_originator(ADDR_A, ADDR_D, QUERY, 0xEEEE, 8);
    route_entry_t route_to_d = fresh_discovered_route(ADDR_D, ADDR_B, 1, 200, 5000);
    bramble_rrep_t rrep = rrep_build_intermediate(&rreq, &route_to_d, ADDR_C, -70, 8);
    TEST_ASSERT_TRUE(rrep_verify(&rrep));
    rrep.hop_count += 1; /* tamper with a MAC-covered field */
    TEST_ASSERT_FALSE(rrep_verify(&rrep));
}

/* --- Integration: intermediate reply short-circuits the flood, mirroring
 * main/mesh_routing.c's handle_rreq control flow (reply, do not also
 * forward). Topology: A -- B -- I -- D was already discovered once (I
 * holds a fresh route to D); a NEW RREQ from E arrives at I via B. I must
 * answer on D's behalf and must NOT schedule a forward, so D never has to
 * see this second discovery at all. --- */
void test_intermediate_reply_suppresses_forward(void) {
    uint32_t now = 20000;
    routing_table_t rt_i;
    route_init(&rt_i);
    /* I already learned a route to D (2 hops away) from an earlier,
     * unrelated discovery. */
    route_entry_t installed = fresh_discovered_route(ADDR_D, ADDR_B, 2, 210, now);
    rt_i.entries[rt_i.count++] = installed;

    /* E originates a fresh discovery for D; by the time it reaches I it has
     * been forwarded once (through some other relay), same shape as
     * test_rrep_build_intermediate_hop_and_metric_math's setup. */
    bramble_rreq_t rreq = rreq_build_originator(ADDR_A, ADDR_D, QUERY2, 0xAAAA, 8);
    bramble_rreq_t at_i = rreq_forward(&rreq, ADDR_B, -75, 6);

    /* Mirrors handle_rreq: dedup + reverse route bookkeeping happen
     * regardless of what follows (not exercised by name here, since this
     * test's focus is the reply-vs-forward decision itself). */
    route_entry_t* cached = route_lookup(&rt_i, at_i.header.dest_addr);
    TEST_ASSERT_NOT_NULL(cached);
    TEST_ASSERT_TRUE(intermediate_rrep_route_usable(cached, now));

    bool forwarded = false; /* would be set true by a schedule_rreq_forward-equivalent call */
    bramble_rrep_t rrep;
    if (intermediate_rrep_route_usable(cached, now)) {
        rrep = rrep_build_intermediate(&at_i, cached, ADDR_C, -75, 6);
        /* Reply path taken: mesh_routing.c's handle_rreq returns here, with NO
         * fall-through to schedule_rreq_forward. */
    } else if (at_i.header.hop_limit > 1) {
        forwarded = true;
    }

    TEST_ASSERT_FALSE(forwarded);
    TEST_ASSERT_TRUE(rrep_verify(&rrep));
    TEST_ASSERT_EQUAL(ADDR_D, rrep.src_addr);
    TEST_ASSERT_EQUAL(ADDR_B, rrep.header.dest_addr); /* back toward the RREQ sender */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_discovery_start_and_lookup);
    RUN_TEST(test_discovery_lookup_by_query);
    RUN_TEST(test_discovery_remove);
    RUN_TEST(test_discovery_table_full);
    RUN_TEST(test_discovery_should_retry);
    RUN_TEST(test_retry_uses_fresh_query_id);
    RUN_TEST(test_rrep_for_any_outstanding_attempt_matches);
    RUN_TEST(test_retry_passes_relay_dedup_within_window);
    RUN_TEST(test_hop_limit_expands_on_retry);
    RUN_TEST(test_forward_jitter_bounds);
    RUN_TEST(test_dedup_expires_with_window);
    RUN_TEST(test_metric_apply_link_penalty_floors_at_zero);
    RUN_TEST(test_rreq_build_originator);
    RUN_TEST(test_rreq_build_originator_expanded_ring);
    RUN_TEST(test_rreq_forward);
    RUN_TEST(test_rreq_forward_hop_count_saturates);
    RUN_TEST(test_rrep_build_destination_hop_count_saturates);
    RUN_TEST(test_rrep_build_destination);
    RUN_TEST(test_rrep_forward);
    RUN_TEST(test_rrep_rx_decide_originator_delivers);
    RUN_TEST(test_rrep_rx_decide_intermediate_forwards);
    RUN_TEST(test_rrep_rx_decide_unsolicited_drops_without_install);
    RUN_TEST(test_three_node_discovery);
    RUN_TEST(test_retry_discovery_succeeds_through_warm_dedup);
    RUN_TEST(test_intermediate_rrep_route_usable_accepts_fresh_discovered_active);
    RUN_TEST(test_intermediate_rrep_route_usable_rejects_null);
    RUN_TEST(test_intermediate_rrep_route_usable_rejects_breadcrumb);
    RUN_TEST(test_intermediate_rrep_route_usable_rejects_stale_and_broken);
    RUN_TEST(test_intermediate_rrep_route_usable_rejects_too_old);
    RUN_TEST(test_rrep_build_intermediate_hop_and_metric_math);
    RUN_TEST(test_rrep_build_intermediate_hop_count_saturates);
    RUN_TEST(test_rrep_build_intermediate_tamper_fails_verify);
    RUN_TEST(test_intermediate_reply_suppresses_forward);
    return UNITY_END();
}
