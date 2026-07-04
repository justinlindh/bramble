#include <string.h>
#include "unity.h"
#include "../components/routing/channel_flood.c"
#include "../components/routing/discovery.c"
#include "../components/routing/routing.c"

void setUp(void) {}
void tearDown(void) {}

/* --- Hop-limit floor: a relay receiving hop_limit <= 1 never forwards,
 * matching forward_data()/RREQ's ">1" convention (hop_limit N means
 * exactly N-hop reach). --- */

void test_hop_limit_exhausted_at_one_does_not_relay(void) {
    channel_flood_decision_t d = channel_flood_decide(1, false, true, 42);
    TEST_ASSERT_FALSE(d.should_relay);
    TEST_ASSERT_EQUAL(0, d.new_hop_limit);
    TEST_ASSERT_EQUAL(0, d.jitter_ms);
}

void test_hop_limit_zero_does_not_relay(void) {
    channel_flood_decision_t d = channel_flood_decide(0, false, true, 42);
    TEST_ASSERT_FALSE(d.should_relay);
}

void test_hop_limit_two_relays_and_decrements_to_one(void) {
    channel_flood_decision_t d = channel_flood_decide(2, false, true, 42);
    TEST_ASSERT_TRUE(d.should_relay);
    TEST_ASSERT_EQUAL(1, d.new_hop_limit);
}

/* --- Duplicate suppression: a broadcast already seen (per the caller's
 * dedup lookup) is never relayed, regardless of hop budget or airtime. --- */

void test_duplicate_does_not_relay(void) {
    channel_flood_decision_t d = channel_flood_decide(8, true, true, 42);
    TEST_ASSERT_FALSE(d.should_relay);
}

void test_duplicate_with_ample_hop_budget_and_free_airtime_still_drops(void) {
    channel_flood_decision_t d = channel_flood_decide(255, true, true, 0);
    TEST_ASSERT_FALSE(d.should_relay);
}

/* --- Airtime-aware relay: a budget-denied node stops relaying rather than
 * amplifying a storm. This is the scale-sensitive lever. --- */

void test_budget_denied_does_not_relay(void) {
    channel_flood_decision_t d = channel_flood_decide(8, false, false, 42);
    TEST_ASSERT_FALSE(d.should_relay);
}

void test_budget_denied_beats_otherwise_healthy_decision(void) {
    /* Plenty of hop budget, not a duplicate -- only the airtime budget
     * says no, and that alone must be enough to stop the relay. */
    channel_flood_decision_t d = channel_flood_decide(8, false, false, 100);
    TEST_ASSERT_FALSE(d.should_relay);
    TEST_ASSERT_EQUAL(0, d.new_hop_limit);
    TEST_ASSERT_EQUAL(0, d.jitter_ms);
}

/* --- Healthy relay path: decrements hop_limit by exactly one and draws
 * jitter from the shared RREQ_FWD_JITTER range (DES-3), not a second
 * hardcoded constant set. --- */

void test_healthy_relay_decrements_hop_limit_by_one(void) {
    channel_flood_decision_t d = channel_flood_decide(8, false, true, 10);
    TEST_ASSERT_TRUE(d.should_relay);
    TEST_ASSERT_EQUAL(7, d.new_hop_limit);
}

void test_healthy_relay_max_hop_limit(void) {
    channel_flood_decision_t d = channel_flood_decide(255, false, true, 10);
    TEST_ASSERT_TRUE(d.should_relay);
    TEST_ASSERT_EQUAL(254, d.new_hop_limit);
}

void test_jitter_reuses_rreq_forward_range(void) {
    /* channel_flood_decide's jitter must be discovery_forward_jitter_ms
     * verbatim (same range, same mapping), not an independent constant
     * set: exercise the same boundary values test_discovery.c uses. */
    channel_flood_decision_t d0 = channel_flood_decide(8, false, true, 0);
    TEST_ASSERT_EQUAL(RREQ_FWD_JITTER_MIN_MS, d0.jitter_ms);

    uint32_t span = RREQ_FWD_JITTER_MAX_MS - RREQ_FWD_JITTER_MIN_MS;
    channel_flood_decision_t dmax = channel_flood_decide(8, false, true, span);
    TEST_ASSERT_EQUAL(RREQ_FWD_JITTER_MAX_MS, dmax.jitter_ms);

    for (uint32_t r = 0; r < 2000; r += 17) {
        channel_flood_decision_t d = channel_flood_decide(8, false, true, r);
        TEST_ASSERT_TRUE(d.jitter_ms >= RREQ_FWD_JITTER_MIN_MS &&
                         d.jitter_ms <= RREQ_FWD_JITTER_MAX_MS);
    }
}

/* --- Flooding F1 rebroadcast suppression (channel_flood_note_overheard):
 * a node cancels its own still-pending flood relay once it has overheard
 * FLOOD_SUPPRESS_AFTER (2) OTHER copies of the SAME src-qualified frame.
 * These exercise the queue-scan helper the dispatch dedup path calls; the
 * "first copy schedules / own retransmit not counted" invariants live in the
 * fact that only DUPLICATE receptions ever reach this helper (proven at the
 * call site, mesh_task.c, and mirrored by the not-miscounted checks here). --- */

/* Schedule-time state: exactly what schedule_flood_relay records for a queued
 * relay -- used, the src-qualified flood_key, heard starting at 0. */
static void queue_one_pending(pending_flood_relay_t* q, uint32_t flood_key) {
    memset(q, 0, sizeof(pending_flood_relay_t) * FLOOD_RELAY_QUEUE_CAPACITY);
    q[0].used = true;
    q[0].flood_key = flood_key;
    q[0].heard = 0;
}

void test_suppress_after_two_overheard_cancels_relay(void) {
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY];
    const uint32_t key = 0xDEADBEEF; /* packet_id ^ src_addr */
    queue_one_pending(q, key);

    /* First overheard copy (the 2nd reception overall): counted, not yet
     * cancelled -- Bramble tolerates one overheard copy (unlike Meshtastic's
     * effective threshold of 1). */
    bool canceled_1 = channel_flood_note_overheard(q, FLOOD_RELAY_QUEUE_CAPACITY, key);
    TEST_ASSERT_FALSE(canceled_1);
    TEST_ASSERT_TRUE(q[0].used);
    TEST_ASSERT_EQUAL(1, q[0].heard);

    /* Second overheard copy: reaches FLOOD_SUPPRESS_AFTER -> cancel (used
     * cleared so process_flood_relay_queue never fires it). */
    bool canceled_2 = channel_flood_note_overheard(q, FLOOD_RELAY_QUEUE_CAPACITY, key);
    TEST_ASSERT_TRUE(canceled_2);
    TEST_ASSERT_FALSE(q[0].used);
    TEST_ASSERT_EQUAL(FLOOD_SUPPRESS_AFTER, q[0].heard);
}

void test_one_overheard_does_not_cancel(void) {
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY];
    const uint32_t key = 0x12345678;
    queue_one_pending(q, key);

    TEST_ASSERT_FALSE(channel_flood_note_overheard(q, FLOOD_RELAY_QUEUE_CAPACITY, key));
    /* Still queued after a single overheard copy: the relay must still fire. */
    TEST_ASSERT_TRUE(q[0].used);
    TEST_ASSERT_EQUAL(1, q[0].heard);
}

void test_overheard_key_is_src_qualified(void) {
    /* flood_key = packet_id ^ src_addr. Same packet_id from a DIFFERENT
     * originator yields a different key and must NOT count against, nor
     * cancel, this node's pending relay -- the whole reason the key is
     * src-qualified rather than packet_id-only. */
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY];
    const uint32_t packet_id = 0x0000AAAA;
    const uint32_t src_a = 0x11111111;
    const uint32_t src_b = 0x22222222;
    const uint32_t key_a = packet_id ^ src_a;
    const uint32_t key_b = packet_id ^ src_b;
    queue_one_pending(q, key_a);

    /* Two overheard copies keyed to the OTHER source: enough to cancel if the
     * key were mismatched-and-still-matched, but here they must be ignored. */
    TEST_ASSERT_FALSE(channel_flood_note_overheard(q, FLOOD_RELAY_QUEUE_CAPACITY, key_b));
    TEST_ASSERT_FALSE(channel_flood_note_overheard(q, FLOOD_RELAY_QUEUE_CAPACITY, key_b));
    TEST_ASSERT_TRUE(q[0].used);
    TEST_ASSERT_EQUAL(0, q[0].heard);

    /* And the correctly-keyed copies still work on the same queue. */
    TEST_ASSERT_FALSE(channel_flood_note_overheard(q, FLOOD_RELAY_QUEUE_CAPACITY, key_a));
    TEST_ASSERT_TRUE(channel_flood_note_overheard(q, FLOOD_RELAY_QUEUE_CAPACITY, key_a));
    TEST_ASSERT_FALSE(q[0].used);
}

void test_overheard_no_pending_entry_is_noop(void) {
    /* No queued relay for this key (node already fired it, or never queued
     * one -- e.g. its OWN origination, which never schedules a self-relay):
     * an overheard copy is a harmless no-op, never a spurious cancel. */
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY];
    memset(q, 0, sizeof(q));
    TEST_ASSERT_FALSE(channel_flood_note_overheard(q, FLOOD_RELAY_QUEUE_CAPACITY, 0xCAFEF00D));
    TEST_ASSERT_FALSE(channel_flood_note_overheard(q, FLOOD_RELAY_QUEUE_CAPACITY, 0xCAFEF00D));
}

void test_overheard_only_touches_matching_slot(void) {
    /* Multiple distinct pending relays coexist; an overheard copy only
     * advances/cancels the ONE whose src-qualified key matches. */
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY];
    memset(q, 0, sizeof(q));
    q[0].used = true;
    q[0].flood_key = 0xAAA0;
    q[1].used = true;
    q[1].flood_key = 0xBBB1;
    q[2].used = true;
    q[2].flood_key = 0xCCC2;

    channel_flood_note_overheard(q, FLOOD_RELAY_QUEUE_CAPACITY, 0xBBB1);
    channel_flood_note_overheard(q, FLOOD_RELAY_QUEUE_CAPACITY, 0xBBB1);

    TEST_ASSERT_TRUE(q[0].used);
    TEST_ASSERT_EQUAL(0, q[0].heard);
    TEST_ASSERT_FALSE(q[1].used); /* cancelled */
    TEST_ASSERT_TRUE(q[2].used);
    TEST_ASSERT_EQUAL(0, q[2].heard);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hop_limit_exhausted_at_one_does_not_relay);
    RUN_TEST(test_hop_limit_zero_does_not_relay);
    RUN_TEST(test_hop_limit_two_relays_and_decrements_to_one);
    RUN_TEST(test_duplicate_does_not_relay);
    RUN_TEST(test_duplicate_with_ample_hop_budget_and_free_airtime_still_drops);
    RUN_TEST(test_budget_denied_does_not_relay);
    RUN_TEST(test_budget_denied_beats_otherwise_healthy_decision);
    RUN_TEST(test_healthy_relay_decrements_hop_limit_by_one);
    RUN_TEST(test_healthy_relay_max_hop_limit);
    RUN_TEST(test_jitter_reuses_rreq_forward_range);
    RUN_TEST(test_suppress_after_two_overheard_cancels_relay);
    RUN_TEST(test_one_overheard_does_not_cancel);
    RUN_TEST(test_overheard_key_is_src_qualified);
    RUN_TEST(test_overheard_no_pending_entry_is_noop);
    RUN_TEST(test_overheard_only_touches_matching_slot);
    return UNITY_END();
}
