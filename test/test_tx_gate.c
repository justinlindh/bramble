#include "unity.h"
#include <stdio.h>
#include <string.h>
#include "tx_gate.h"

/*
 * Chokepoint gating tests (DES-5, DES-6): every transmission must pass
 * budget check -> LBT -> transmit -> debit, with a fake radio recording
 * exactly what reaches the air.
 */

void setUp(void);
void tearDown(void) {}

/* ── Fake radio / environment ───────────────────────────────────────── */

#define FAKE_MAX_TX 64

static struct {
    int tx_count;
    uint8_t tx_len[FAKE_MAX_TX];
    int tx_rc; /* what the radio returns */
    int busy_responses;
    int cad_calls;
    int delay_calls;
    uint32_t delay_total_ms;
    uint32_t now;
} s_fake;

static bool fake_busy(void) {
    s_fake.cad_calls++;
    if (s_fake.busy_responses > 0) {
        s_fake.busy_responses--;
        return true;
    }
    return false;
}
static int fake_tx(const uint8_t* d, uint8_t l) {
    (void)d;
    if (s_fake.tx_rc == 0 && s_fake.tx_count < FAKE_MAX_TX)
        s_fake.tx_len[s_fake.tx_count] = l;
    if (s_fake.tx_rc == 0)
        s_fake.tx_count++;
    return s_fake.tx_rc;
}
static void fake_toa_params(uint8_t* sf, uint32_t* bw_hz, uint8_t* cr) {
    *sf = 9;
    *bw_hz = 125000;
    *cr = 1;
}
static uint32_t fake_now(void) { return s_fake.now; }
static uint32_t fake_rand(void) { return 7; }
static void fake_delay(uint32_t ms) {
    s_fake.delay_calls++;
    s_fake.delay_total_ms += ms;
    s_fake.now += ms;
}

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

static void drain_tier(uint8_t tier) {
    uint32_t left = airtime_budget_remaining(&s_gate.budget, tier);
    airtime_budget_debit(&s_gate.budget, tier, left);
}

/* ── Tier mapping ───────────────────────────────────────────────────── */

void test_kind_tier_mapping(void) {
    TEST_ASSERT_EQUAL_UINT8(AIRTIME_TIER_NORMAL, tx_gate_kind_tier(TX_KIND_DATA));
    TEST_ASSERT_EQUAL_UINT8(AIRTIME_TIER_NORMAL, tx_gate_kind_tier(TX_KIND_DATA_RETRY));
    TEST_ASSERT_EQUAL_UINT8(AIRTIME_TIER_NORMAL, tx_gate_kind_tier(TX_KIND_FORWARD));
    TEST_ASSERT_EQUAL_UINT8(AIRTIME_TIER_NORMAL, tx_gate_kind_tier(TX_KIND_MAILBOX));
    TEST_ASSERT_EQUAL_UINT8(AIRTIME_TIER_NORMAL, tx_gate_kind_tier(TX_KIND_PROBE_REPLY));
    TEST_ASSERT_EQUAL_UINT8(AIRTIME_TIER_CRITICAL, tx_gate_kind_tier(TX_KIND_ROUTING));
    TEST_ASSERT_EQUAL_UINT8(AIRTIME_TIER_CRITICAL, tx_gate_kind_tier(TX_KIND_ACK));
    TEST_ASSERT_EQUAL_UINT8(AIRTIME_TIER_BROADCAST, tx_gate_kind_tier(TX_KIND_BEACON));
    TEST_ASSERT_EQUAL_UINT8(AIRTIME_TIER_BROADCAST, tx_gate_kind_tier(TX_KIND_DATA_BROADCAST));
    TEST_ASSERT_EQUAL_UINT8(AIRTIME_TIER_BROADCAST, tx_gate_kind_tier(TX_KIND_PROBE));
    TEST_ASSERT_EQUAL_UINT8(AIRTIME_TIER_RECEIPT, tx_gate_kind_tier(TX_KIND_RECEIPT));
}

/* ── Budget gating ──────────────────────────────────────────────────── */

void test_success_transmits_and_debits(void) {
    uint8_t pkt[40] = {0};
    uint32_t before = airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_NORMAL);
    uint32_t cost = tx_gate_cost_ms(&s_gate, sizeof(pkt));

    TEST_ASSERT_EQUAL_INT(TX_GATE_OK, tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA));
    TEST_ASSERT_EQUAL_INT(1, s_fake.tx_count);
    TEST_ASSERT_EQUAL_UINT8(sizeof(pkt), s_fake.tx_len[0]);
    TEST_ASSERT_EQUAL_UINT32(before - cost,
                             airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_NORMAL));
}

void test_denied_when_budget_exhausted_radio_untouched(void) {
    drain_tier(AIRTIME_TIER_RECEIPT);
    uint8_t pkt[40] = {0};
    TEST_ASSERT_EQUAL_INT(TX_GATE_ERR_BUDGET,
                          tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_RECEIPT));
    /* Denial must short-circuit before LBT and before the radio. */
    TEST_ASSERT_EQUAL_INT(0, s_fake.tx_count);
    TEST_ASSERT_EQUAL_INT(0, s_fake.cad_calls);
}

void test_tiers_are_isolated(void) {
    /* Draining BROADCAST denies beacons but unicast data still flows. */
    drain_tier(AIRTIME_TIER_BROADCAST);
    uint8_t pkt[36] = {0};
    TEST_ASSERT_EQUAL_INT(TX_GATE_ERR_BUDGET,
                          tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_BEACON));
    TEST_ASSERT_EQUAL_INT(TX_GATE_OK, tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA));
    TEST_ASSERT_EQUAL_INT(1, s_fake.tx_count);
}

void test_routing_reserve_survives_data_exhaustion(void) {
    /* The CRITICAL lane is reserved for control: drain NORMAL completely,
     * routing packets must still transmit. */
    drain_tier(AIRTIME_TIER_NORMAL);
    uint8_t data_pkt[80] = {0};
    uint8_t rreq[30] = {0};
    TEST_ASSERT_EQUAL_INT(TX_GATE_ERR_BUDGET,
                          tx_gate_transmit(&s_gate, data_pkt, sizeof(data_pkt), TX_KIND_DATA));
    TEST_ASSERT_EQUAL_INT(TX_GATE_OK,
                          tx_gate_transmit(&s_gate, rreq, sizeof(rreq), TX_KIND_ROUTING));
    TEST_ASSERT_EQUAL_INT(TX_GATE_OK, tx_gate_transmit(&s_gate, rreq, sizeof(rreq), TX_KIND_ACK));
}

void test_critical_borrows_from_normal_when_drained(void) {
    drain_tier(AIRTIME_TIER_CRITICAL);
    uint8_t rerr[24] = {0};
    /* CRITICAL empty, NORMAL full: control borrows and still goes out. */
    TEST_ASSERT_EQUAL_INT(TX_GATE_OK,
                          tx_gate_transmit(&s_gate, rerr, sizeof(rerr), TX_KIND_ROUTING));
    /* Both empty: denied. */
    drain_tier(AIRTIME_TIER_NORMAL);
    drain_tier(AIRTIME_TIER_CRITICAL);
    TEST_ASSERT_EQUAL_INT(TX_GATE_ERR_BUDGET,
                          tx_gate_transmit(&s_gate, rerr, sizeof(rerr), TX_KIND_ROUTING));
}

void test_radio_failure_does_not_debit(void) {
    s_fake.tx_rc = -1;
    uint8_t pkt[40] = {0};
    uint32_t before = airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_NORMAL);
    TEST_ASSERT_EQUAL_INT(TX_GATE_ERR_RADIO,
                          tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA));
    TEST_ASSERT_EQUAL_UINT32(before, airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_NORMAL));
}

void test_can_transmit_precheck_does_not_debit(void) {
    uint32_t before = airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_BROADCAST);
    TEST_ASSERT_TRUE(tx_gate_can_transmit(&s_gate, 100, TX_KIND_DATA_BROADCAST));
    TEST_ASSERT_EQUAL_UINT32(before,
                             airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_BROADCAST));
    TEST_ASSERT_EQUAL_INT(0, s_fake.tx_count);

    drain_tier(AIRTIME_TIER_BROADCAST);
    TEST_ASSERT_FALSE(tx_gate_can_transmit(&s_gate, 100, TX_KIND_DATA_BROADCAST));
}

/* ── LBT behavior ───────────────────────────────────────────────────── */

void test_lbt_backs_off_then_transmits(void) {
    s_fake.busy_responses = 2; /* busy twice, then clear */
    uint8_t pkt[20] = {0};
    TEST_ASSERT_EQUAL_INT(TX_GATE_OK, tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA));
    TEST_ASSERT_EQUAL_INT(3, s_fake.cad_calls);   /* busy, busy, clear */
    TEST_ASSERT_EQUAL_INT(2, s_fake.delay_calls); /* backed off twice */
    TEST_ASSERT_EQUAL_INT(1, s_fake.tx_count);
}

void test_lbt_gives_up_and_transmits_after_max_attempts(void) {
    /* Anti-starvation: a permanently busy channel still transmits after
     * 3 CAD attempts (deliberate, pre-existing policy). */
    s_fake.busy_responses = 100;
    uint8_t pkt[20] = {0};
    TEST_ASSERT_EQUAL_INT(TX_GATE_OK, tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA));
    TEST_ASSERT_EQUAL_INT(3, s_fake.cad_calls);
    TEST_ASSERT_EQUAL_INT(3, s_fake.delay_calls);
    TEST_ASSERT_EQUAL_INT(1, s_fake.tx_count);
}

void test_lbt_skipped_entirely_when_budget_denies(void) {
    s_fake.busy_responses = 100;
    drain_tier(AIRTIME_TIER_NORMAL);
    uint8_t pkt[20] = {0};
    TEST_ASSERT_EQUAL_INT(TX_GATE_ERR_BUDGET,
                          tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA));
    TEST_ASSERT_EQUAL_INT(0, s_fake.cad_calls);
    TEST_ASSERT_EQUAL_INT(0, s_fake.delay_calls);
}

/* ── Refill interaction ─────────────────────────────────────────────── */

void test_denied_then_allowed_after_refill(void) {
    drain_tier(AIRTIME_TIER_NORMAL);
    uint8_t pkt[20] = {0};
    TEST_ASSERT_EQUAL_INT(TX_GATE_ERR_BUDGET,
                          tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA));
    /* Tokens accrue continuously; minutes later the same packet passes.
     * (CRITICAL borrow is blocked by also draining it, isolating NORMAL.) */
    drain_tier(AIRTIME_TIER_CRITICAL);
    s_fake.now += 10u * 60u * 1000u; /* 10 minutes */
    TEST_ASSERT_EQUAL_INT(TX_GATE_OK, tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA));
}

/* A remote RREQ flood (CRITICAL via TX_KIND_ROUTING) cannot exhaust the
 * local data lane: borrow is capped, so NORMAL keeps >= 75% and user
 * sends keep working. Forward-side RREQ rate limiting lands in
 * workstream 1.3; this bound holds regardless. */
void test_routing_flood_cannot_exhaust_data_lane(void) {
    uint8_t rreq[30] = {0};
    uint8_t data[120] = {0};
    uint32_t normal_max = s_gate.budget.max_ms[AIRTIME_IDX_NORMAL];

    for (int i = 0; i < 2000; i++)
        (void)tx_gate_transmit(&s_gate, rreq, sizeof(rreq), TX_KIND_ROUTING);

    TEST_ASSERT_TRUE(airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_NORMAL) >=
                     (normal_max * 3u) / 4u);
    TEST_ASSERT_EQUAL_INT(TX_GATE_OK, tx_gate_transmit(&s_gate, data, sizeof(data), TX_KIND_DATA));
}

void test_mesh_size_propagates_to_budget(void) {
    tx_gate_set_mesh_size(&s_gate, 50);
    TEST_ASSERT_EQUAL_UINT8(50, s_gate.budget.profile_peer_count);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_kind_tier_mapping);
    RUN_TEST(test_success_transmits_and_debits);
    RUN_TEST(test_denied_when_budget_exhausted_radio_untouched);
    RUN_TEST(test_tiers_are_isolated);
    RUN_TEST(test_routing_reserve_survives_data_exhaustion);
    RUN_TEST(test_critical_borrows_from_normal_when_drained);
    RUN_TEST(test_radio_failure_does_not_debit);
    RUN_TEST(test_can_transmit_precheck_does_not_debit);
    RUN_TEST(test_lbt_backs_off_then_transmits);
    RUN_TEST(test_lbt_gives_up_and_transmits_after_max_attempts);
    RUN_TEST(test_lbt_skipped_entirely_when_budget_denies);
    RUN_TEST(test_denied_then_allowed_after_refill);
    RUN_TEST(test_routing_flood_cannot_exhaust_data_lane);
    RUN_TEST(test_mesh_size_propagates_to_budget);
    return UNITY_END();
}
