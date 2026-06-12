#include "unity.h"
#include <stdio.h>
#include <string.h>
#include "tx_gate.h"

/*
 * Integration-style retry-storm tests (DES-6): ACK-driven retransmissions
 * pass the same gate as everything else, so a retry burst is physically
 * incapable of exceeding the airtime budget. Also pins the deliberate
 * deny-burns-attempt policy used by the mesh retry scheduler.
 */

void setUp(void);
void tearDown(void) {}

static struct {
    int tx_count;
    uint32_t now;
} s_fake;

static bool fake_busy(void) { return false; }
static int fake_tx(const uint8_t* d, uint8_t l) {
    (void)d;
    (void)l;
    s_fake.tx_count++;
    return 0;
}
static void fake_toa_params(uint8_t* sf, uint32_t* bw_hz, uint8_t* cr) {
    *sf = 9;
    *bw_hz = 125000;
    *cr = 1;
}
static uint32_t fake_now(void) { return s_fake.now; }
static uint32_t fake_rand(void) { return 3; }
static void fake_delay(uint32_t ms) { s_fake.now += ms; }

static tx_gate_t s_gate;

void setUp(void) {
    memset(&s_fake, 0, sizeof(s_fake));
    tx_gate_ops_t ops = {
        .channel_busy = fake_busy,
        .transmit = fake_tx,
        .get_toa_params = fake_toa_params,
        .now_ms = fake_now,
        .random_u32 = fake_rand,
        .delay_ms = fake_delay,
        .wdt_feed = NULL,
    };
    tx_gate_init(&s_gate, &ops, 100, false);
}

/* A tight burst of retransmission attempts transmits only while tokens
 * last; everything after that is denied with the radio untouched. */
void test_retry_burst_cannot_exceed_budget(void) {
    uint8_t pkt[200] = {0};
    uint32_t cost = tx_gate_cost_ms(&s_gate, sizeof(pkt));
    uint32_t capacity = airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_NORMAL);

    int sent = 0, denied = 0;
    for (int i = 0; i < 500; i++) { /* storm: 500 back-to-back attempts */
        int rc = tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA_RETRY);
        if (rc == TX_GATE_OK)
            sent++;
        else if (rc == TX_GATE_ERR_BUDGET)
            denied++;
    }

    TEST_ASSERT_EQUAL_INT(sent, s_fake.tx_count);
    TEST_ASSERT_EQUAL_INT(500, sent + denied);
    /* Total airtime on the air never exceeds the bucket. */
    TEST_ASSERT_TRUE((uint64_t)sent * cost <= capacity);
    /* And the budget actually denied the tail of the storm. */
    TEST_ASSERT_TRUE(denied > 0);
}

/* Mirror of the mesh retry scheduler policy: bounded attempts, exponential
 * backoff, and a denial burns the attempt (deliberate: the original TX
 * already went out; burning bounds failure latency so the sender reports
 * FAILED instead of leaving a zombie pending entry). The packet must reach
 * a terminal state within max_attempts regardless of budget. */
void test_scheduler_with_denials_terminates(void) {
    uint8_t pkt[200] = {0};

    const uint8_t max_attempts = 8;
    uint8_t attempt = 0;
    uint32_t next_retry_ms = 0;
    int gate_calls = 0;
    bool failed = false;

    /* 1s scheduler ticks, like mesh_periodic_maintenance. The mesh stays
     * saturated for the whole window: other traffic consumes every token
     * the refill produces, so each retry attempt is denied. */
    for (uint32_t t = 0; t <= 1200u * 1000u && !failed; t += 1000u) {
        s_fake.now = t;
        airtime_budget_refill(&s_gate.budget, t);
        airtime_budget_debit(&s_gate.budget, AIRTIME_TIER_NORMAL,
                             airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_NORMAL));
        airtime_budget_debit(&s_gate.budget, AIRTIME_TIER_CRITICAL,
                             airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_CRITICAL));
        if (t < next_retry_ms)
            continue;
        if (attempt >= max_attempts) {
            failed = true; /* MSG_STATUS_FAILED */
            break;
        }
        int rc = tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA_RETRY);
        gate_calls++;
        TEST_ASSERT_EQUAL_INT(TX_GATE_ERR_BUDGET, rc);
        attempt++; /* denial burns the attempt, same as mesh_task */
        next_retry_ms = t + (1000u << attempt);
    }

    TEST_ASSERT_TRUE(failed);
    /* Bounded: exactly max_attempts gate calls, zero radio transmissions. */
    TEST_ASSERT_EQUAL_INT(max_attempts, gate_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.tx_count);
}

/* Under partial budget, a retry sequence transmits what the bucket allows
 * and is denied for the rest; the sum of transmitted ToA stays bounded. */
void test_partial_budget_retry_sequence(void) {
    uint8_t pkt[120] = {0};
    uint32_t cost = tx_gate_cost_ms(&s_gate, sizeof(pkt));

    /* Leave room for exactly 3 transmissions in NORMAL; block borrow. */
    uint32_t keep = cost * 3u;
    uint32_t have = airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_NORMAL);
    airtime_budget_debit(&s_gate.budget, AIRTIME_TIER_NORMAL, have - keep);

    int sent = 0;
    for (int i = 0; i < 8; i++) {
        if (tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA_RETRY) == TX_GATE_OK)
            sent++;
    }
    TEST_ASSERT_EQUAL_INT(3, sent);
    TEST_ASSERT_EQUAL_INT(3, s_fake.tx_count);
}

/* Retries share the NORMAL lane with fresh sends: they cannot raid the
 * CRITICAL control reserve. */
void test_retry_storm_does_not_starve_control(void) {
    uint8_t pkt[200] = {0};
    for (int i = 0; i < 500; i++)
        (void)tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA_RETRY);

    uint8_t rreq[30] = {0};
    TEST_ASSERT_EQUAL_INT(TX_GATE_OK,
                          tx_gate_transmit(&s_gate, rreq, sizeof(rreq), TX_KIND_ROUTING));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_retry_burst_cannot_exceed_budget);
    RUN_TEST(test_scheduler_with_denials_terminates);
    RUN_TEST(test_partial_budget_retry_sequence);
    RUN_TEST(test_retry_storm_does_not_starve_control);
    return UNITY_END();
}
