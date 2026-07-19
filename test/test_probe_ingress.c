/*
 * PROBE ingress backpressure (issue #75).
 *
 * These are regression tests, not characterization tests. Before the fix,
 * handle_probe did one length check and nothing else: every inbound probe
 * bought a three-send PROBE_ACK burst plus a rebroadcast, from any source,
 * at any rate, forever. There was no bound to assert, so every "bounded"
 * assertion below fails against that behaviour. probe_ingress_allow is the
 * only thing standing between a received probe and those transmissions, so
 * deleting it makes reply/forward unconditionally true and this file goes
 * red. That was verified by reverting the fix, not assumed.
 *
 * The design decision under test is that the buckets are NODE-GLOBAL and not
 * keyed on the probe's src_addr. src_addr is an unauthenticated wire field:
 * keying on it is evadable by rotation (so it adds no ceiling) and hands an
 * attacker a targeted DoS against any address it forges (so it subtracts
 * safety). This follows SEC-M4's forwarded-RREQ cap. The last group of tests
 * pins that property down directly, because the intuitive "per-sender token
 * bucket" fix is exactly what must not creep back in.
 */

#include <string.h>

#include "unity.h"

#include "../components/security/security.c"

void setUp(void) {}
void tearDown(void) {}

/* --- The bound. A probe flood buys a bounded number of transmissions. --- */

void test_probe_flood_reply_burst_is_bounded(void) {
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0);

    int replies = 0;
    /* 1000 probes inside a single millisecond: no refill can occur. */
    for (int i = 0; i < 1000; i++) {
        if (probe_ingress_allow(&rl, 0).reply)
            replies++;
    }

    TEST_ASSERT_EQUAL_UINT32(PROBE_REPLY_BURST, (uint32_t)replies);
    TEST_ASSERT_EQUAL_UINT32(PROBE_REPLY_BURST, rl.accepted);
    TEST_ASSERT_EQUAL_UINT32(1000 - PROBE_REPLY_BURST, rl.dropped_reply);
}

void test_probe_flood_stays_bounded_over_a_long_window(void) {
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0);

    /* Ten minutes of sustained pressure, one probe every 100ms: 6000 frames
     * offered. Accepted must track the REFILL rate, not the arrival rate. */
    int replies = 0;
    for (uint32_t t = 0; t < 600000; t += 100) {
        if (probe_ingress_allow(&rl, t).reply)
            replies++;
    }

    uint32_t expected = PROBE_REPLY_BURST + (600000 / PROBE_REPLY_REFILL_MS);
    TEST_ASSERT_UINT32_WITHIN(2, expected, (uint32_t)replies);
    /* Sanity: a tiny fraction of the 6000 frames offered. */
    TEST_ASSERT_TRUE(replies < 200);
}

void test_probe_flood_rebroadcast_is_bounded_harder_than_the_reply(void) {
    /* Forwarding is the term that multiplies across the mesh, so it must run
     * out first: propagation stops while the node still answers neighbors. */
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0);

    int replies = 0, forwards = 0;
    for (int i = 0; i < 1000; i++) {
        probe_ingress_decision_t d = probe_ingress_allow(&rl, 0);
        if (d.reply)
            replies++;
        if (d.forward)
            forwards++;
    }

    TEST_ASSERT_EQUAL_UINT32(PROBE_FWD_BURST, (uint32_t)forwards);
    TEST_ASSERT_TRUE(forwards < replies);
    TEST_ASSERT_EQUAL_UINT32(PROBE_REPLY_BURST - PROBE_FWD_BURST, rl.dropped_forward);
}

void test_total_transmissions_bought_by_a_flood_are_bounded(void) {
    /* The headline number from issue #75. Each answered probe costs a
     * three-send reply burst, each forwarded one costs a rebroadcast. Under
     * the old code 1000 injected frames bought 4000 transmissions; here the
     * ceiling is a small constant regardless of how many frames arrive. */
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0);

    int transmissions = 0;
    for (int i = 0; i < 1000; i++) {
        probe_ingress_decision_t d = probe_ingress_allow(&rl, 0);
        if (d.reply)
            transmissions += 3;
        if (d.forward)
            transmissions += 1;
    }

    TEST_ASSERT_EQUAL_INT(3 * PROBE_REPLY_BURST + PROBE_FWD_BURST, transmissions);
    TEST_ASSERT_TRUE(transmissions < 4000 / 100);
}

void test_forward_is_never_granted_without_a_reply(void) {
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0);

    for (int i = 0; i < 500; i++) {
        probe_ingress_decision_t d = probe_ingress_allow(&rl, i * 37);
        if (d.forward) {
            TEST_ASSERT_TRUE(d.reply);
        }
    }
}

/* --- Normal use survives. A rate limit that breaks the feature is not a
 * fix, and PROBE has to keep working for unprovisioned nodes. --- */

void test_a_normal_probe_sweep_is_never_throttled(void) {
    /* A sweep is a handful of rounds seconds apart. Several neighbors all
     * sweeping at once must still be answered from the initial burst. */
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0);

    for (int round = 0; round < PROBE_REPLY_BURST; round++) {
        TEST_ASSERT_TRUE(probe_ingress_allow(&rl, (uint32_t)round * 2000).reply);
    }
    TEST_ASSERT_EQUAL_UINT32(0, rl.dropped_reply);
}

void test_the_limiter_self_heals_without_operator_action(void) {
    /* The accepted cost of a global cap is that a flood delays legitimate
     * probes. It must not OUTLAST the flood: one refill window after the
     * bucket empties, probing works again with nothing reset by hand. */
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0);

    for (int i = 0; i < 500; i++) {
        probe_ingress_allow(&rl, 0);
    }
    TEST_ASSERT_FALSE(probe_ingress_allow(&rl, 0).reply);

    TEST_ASSERT_TRUE(probe_ingress_allow(&rl, PROBE_REPLY_REFILL_MS).reply);
}

/* --- The design property: no per-sender keying. A caller cannot express
 * "this probe came from X", so a forged src_addr cannot single anyone out.
 * Under a flood every sender is refused identically, which is uniform rather
 * than targetable. --- */

void test_the_limiter_takes_no_sender_identity_at_all(void) {
    /* Two calls that differ only in who "sent" the probe are indistinguish-
     * able, because there is no place to put a sender. If a src_addr
     * parameter is ever added, this file stops compiling, which is the
     * intended tripwire. */
    probe_ingress_limiter_t victim_run;
    probe_ingress_limiter_t attacker_run;
    probe_ingress_init(&victim_run, 0);
    probe_ingress_init(&attacker_run, 0);

    for (int i = 0; i < 100; i++) {
        probe_ingress_allow(&victim_run, 0);
        probe_ingress_allow(&attacker_run, 0);
    }

    TEST_ASSERT_EQUAL_UINT32(victim_run.accepted, attacker_run.accepted);
    TEST_ASSERT_EQUAL_UINT32(victim_run.dropped_reply, attacker_run.dropped_reply);
    TEST_ASSERT_EQUAL_MEMORY(&victim_run, &attacker_run, sizeof(victim_run));
}

void test_a_forged_source_flood_cannot_silence_a_legitimate_sender_beyond_refill(void) {
    /* The forged-source DoS this design exists to avoid. An attacker floods,
     * spoofing whatever src_addr it likes, including a victim's. Because the
     * bucket is not keyed on the claim, the attacker cannot drain a
     * victim-specific budget: the only thing it can drain is the shared
     * ceiling, which every honest sender then shares equally and which
     * refills on a fixed schedule the attacker does not control.
     *
     * A per-sender design fails this test: the victim's bucket would still be
     * empty at t = PROBE_REPLY_REFILL_MS because the attacker drained a
     * bucket sized and timed independently of the global one. */
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0);

    /* Attacker empties the ceiling at t=0, forging sources freely. */
    for (int i = 0; i < 10000; i++) {
        probe_ingress_allow(&rl, 0);
    }

    /* The legitimate sender is served on the very next refill tick, and the
     * attacker gained nothing by aiming at them specifically. */
    TEST_ASSERT_TRUE(probe_ingress_allow(&rl, PROBE_REPLY_REFILL_MS).reply);
}

void test_sustained_forged_flood_still_leaves_a_share_for_a_legitimate_sender(void) {
    /* The attacker never stops: it sends 100 probes per refill window. A
     * legitimate sender interleaves one probe per window. Because refusal is
     * blind to the claimed source, the honest sender still lands probes at
     * roughly the refill rate rather than being locked out. */
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0);

    int honest_replies = 0;
    for (int window = 0; window < 20; window++) {
        uint32_t t = (uint32_t)window * PROBE_REPLY_REFILL_MS;
        /* Honest sender goes first in the window, then the flood. */
        if (probe_ingress_allow(&rl, t).reply)
            honest_replies++;
        for (int i = 0; i < 100; i++) {
            probe_ingress_allow(&rl, t);
        }
    }

    /* One token per window is earned, and the honest probe claims it. */
    TEST_ASSERT_TRUE(honest_replies >= 19);
}

/* --- Refill mechanics the bounds above depend on. --- */

void test_refill_carries_the_sub_window_remainder(void) {
    /* A node polled every millisecond must not accrue tokens faster than one
     * polled once, or the ceiling becomes a function of the attacker's send
     * rate. */
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0);
    rl.reply.tokens = 0;

    for (uint32_t t = 0; t < PROBE_REPLY_REFILL_MS; t++) {
        TEST_ASSERT_FALSE(probe_ingress_allow(&rl, t).reply);
    }
    TEST_ASSERT_TRUE(probe_ingress_allow(&rl, PROBE_REPLY_REFILL_MS).reply);
}

void test_refill_never_exceeds_the_burst_ceiling(void) {
    /* A long quiet period must not bank unlimited tokens for one huge burst. */
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0);
    rl.reply.tokens = 0;

    uint32_t quiet = 100u * PROBE_REPLY_REFILL_MS;
    int replies = 0;
    for (int i = 0; i < 1000; i++) {
        if (probe_ingress_allow(&rl, quiet).reply)
            replies++;
    }
    TEST_ASSERT_EQUAL_INT(PROBE_REPLY_BURST, replies);
}

void test_millisecond_clock_rollover_does_not_stall_probe_handling(void) {
    /* A mesh node passes 2^32 ms of uptime in about 49 days. Naive
     * subtraction there would stall probes for weeks. */
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0xFFFFFF00u);
    rl.reply.tokens = 0;

    uint32_t after_wrap = 0xFFFFFF00u + PROBE_REPLY_REFILL_MS; /* wraps past 0 */
    TEST_ASSERT_TRUE(probe_ingress_allow(&rl, after_wrap).reply);
}

void test_init_starts_both_buckets_full_with_zeroed_counters(void) {
    probe_ingress_limiter_t rl;
    memset(&rl, 0xFF, sizeof(rl));
    probe_ingress_init(&rl, 4242);

    TEST_ASSERT_EQUAL_UINT32(PROBE_REPLY_BURST, rl.reply.tokens);
    TEST_ASSERT_EQUAL_UINT32(PROBE_FWD_BURST, rl.forward.tokens);
    TEST_ASSERT_EQUAL_UINT32(4242, rl.reply.last_refill_ms);
    TEST_ASSERT_EQUAL_UINT32(4242, rl.forward.last_refill_ms);
    TEST_ASSERT_EQUAL_UINT32(0, rl.accepted);
    TEST_ASSERT_EQUAL_UINT32(0, rl.dropped_reply);
    TEST_ASSERT_EQUAL_UINT32(0, rl.dropped_forward);
}

void test_counters_account_for_every_probe_offered(void) {
    /* The diagnostics surface must not lose probes: every offered frame is
     * either accepted or counted as a reply drop. */
    probe_ingress_limiter_t rl;
    probe_ingress_init(&rl, 0);

    for (int i = 0; i < 777; i++) {
        probe_ingress_allow(&rl, 0);
    }

    TEST_ASSERT_EQUAL_UINT32(777, rl.accepted + rl.dropped_reply);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_probe_flood_reply_burst_is_bounded);
    RUN_TEST(test_probe_flood_stays_bounded_over_a_long_window);
    RUN_TEST(test_probe_flood_rebroadcast_is_bounded_harder_than_the_reply);
    RUN_TEST(test_total_transmissions_bought_by_a_flood_are_bounded);
    RUN_TEST(test_forward_is_never_granted_without_a_reply);
    RUN_TEST(test_a_normal_probe_sweep_is_never_throttled);
    RUN_TEST(test_the_limiter_self_heals_without_operator_action);
    RUN_TEST(test_the_limiter_takes_no_sender_identity_at_all);
    RUN_TEST(test_a_forged_source_flood_cannot_silence_a_legitimate_sender_beyond_refill);
    RUN_TEST(test_sustained_forged_flood_still_leaves_a_share_for_a_legitimate_sender);
    RUN_TEST(test_refill_carries_the_sub_window_remainder);
    RUN_TEST(test_refill_never_exceeds_the_burst_ceiling);
    RUN_TEST(test_millisecond_clock_rollover_does_not_stall_probe_handling);
    RUN_TEST(test_init_starts_both_buckets_full_with_zeroed_counters);
    RUN_TEST(test_counters_account_for_every_probe_offered);
    return UNITY_END();
}
