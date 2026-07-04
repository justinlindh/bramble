#include "unity.h"
#include <stdio.h>
#include <string.h>
#include "tx_gate.h"
#include "freq_plan.h"

/*
 * Beacon-fit cross-check (adversarial review BLOCKER on PR #82).
 *
 * Under the EU868 duty cap the BROADCAST lane refills far less per hour
 * than a 60s beacon cadence costs, so beacons must fit the budget BY
 * DESIGN: the gate computes a minimum interval at which beacon ToA demand
 * fits TX_GATE_BEACON_LANE_PCT of the lane refill, the scheduler
 * stretches to it, and a one-beacon reserve stops broadcast data from
 * draining the lane below the next beacon's cost.
 *
 * These tests sweep every supported SF and every region profile and
 * assert: (1) the effective cadence's hourly ToA demand fits the lane
 * share that funds it, (2) the US915 default cadence is unchanged at the
 * default SF, (3) on EU868 at the live SF9 default the stretched cadence
 * stays inside NEIGHBOR_EXPIRY_MS so nodes do not vanish from neighbor
 * tables, and (4) a greedy broadcast-data flood can never make a beacon
 * miss its slot (the liveness regression the review caught).
 */

void setUp(void);
void tearDown(void) {}

#define BEACON_WIRE_LEN 40u        /* base beacon + short name */
#define CONFIGURED_INTERVAL 60000u /* BEACON_INTERVAL_MS default */
#define NEIGHBOR_EXPIRY 600000u    /* routing.h NEIGHBOR_EXPIRY_MS */

static struct {
    uint8_t sf;
    uint32_t now;
    int tx_count;
    int beacon_denied;
} s_fake;

static bool fake_busy(void) { return false; }
static int fake_tx(const uint8_t* d, uint8_t l) {
    (void)d;
    (void)l;
    s_fake.tx_count++;
    return 0;
}
static void fake_toa_params(uint8_t* sf, uint32_t* bw_hz, uint8_t* cr) {
    *sf = s_fake.sf;
    *bw_hz = 125000;
    *cr = 1;
}
static uint32_t fake_now(void) { return s_fake.now; }
static uint32_t fake_rand(void) { return 1; }
static void fake_delay(uint32_t ms) { s_fake.now += ms; }

static tx_gate_t s_gate;

void setUp(void) {
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.sf = 9;
}

static void gate_for(const bramble_freq_plan_t* plan, uint8_t sf, uint8_t peers) {
    s_fake.sf = sf;
    tx_gate_ops_t ops = {
        .channel_busy = fake_busy,
        .transmit = fake_tx,
        .get_toa_params = fake_toa_params,
        .now_ms = fake_now,
        .random_u32 = fake_rand,
        .delay_ms = fake_delay,
        .wdt_feed = NULL,
    };
    tx_gate_init(&s_gate, &ops, plan->max_duty_cycle_pct, plan->duty_cycle_enforced);
    tx_gate_set_mesh_size(&s_gate, peers);
    tx_gate_set_beacon_profile(&s_gate, BEACON_WIRE_LEN);
}

/* (1) Every SF x region x mesh-size profile: effective cadence demand
 * fits the beacon share of the lane that funds it. */
void test_beacon_demand_fits_lane_at_every_sf_and_region(void) {
    const uint8_t sfs[] = {7, 8, 9, 10, 11, 12};
    const uint8_t peers[] = {0, 12, 20, 50};
    for (int r = 0; r < FREQ_REGION_COUNT; r++) {
        const bramble_freq_plan_t* plan = freq_plan_get((bramble_freq_region_t)r);
        for (size_t s = 0; s < sizeof(sfs); s++) {
            for (size_t p = 0; p < sizeof(peers); p++) {
                gate_for(plan, sfs[s], peers[p]);
                uint32_t cost = tx_gate_cost_ms(&s_gate, BEACON_WIRE_LEN);
                uint32_t min_iv = tx_gate_min_beacon_interval_ms(&s_gate);
                uint32_t effective = (min_iv > CONFIGURED_INTERVAL) ? min_iv : CONFIGURED_INTERVAL;

                uint64_t demand_per_hr = ((uint64_t)cost * AIRTIME_REFILL_INTERVAL_MS) / effective;
                uint64_t lane_share = ((uint64_t)s_gate.budget.max_ms[AIRTIME_IDX_BROADCAST] *
                                       TX_GATE_BEACON_LANE_PCT) /
                                      100u;

                uint64_t lane_full = s_gate.budget.max_ms[AIRTIME_IDX_BROADCAST];
                char msg[96];
                snprintf(msg, sizeof(msg), "%s SF%u peers=%u demand=%u share=%u", plan->name,
                         (unsigned)sfs[s], (unsigned)peers[p], (unsigned)demand_per_hr,
                         (unsigned)lane_share);
                /* one beacon of slack covers integer rounding of the floor;
                 * min_iv == ceiling means the liveness clamp engaged */
                if (min_iv < TX_GATE_BEACON_LIVENESS_CEILING_MS) {
                    /* share-derived floor: cadence fits the beacon share */
                    TEST_ASSERT_TRUE_MESSAGE(demand_per_hr <= lane_share + cost, msg);
                } else {
                    /* liveness-ceiling regime: beacons may take the whole
                     * lane, but never more than it refills */
                    TEST_ASSERT_TRUE_MESSAGE(demand_per_hr <= lane_full + cost, msg);
                }
            }
        }
    }
}

/* (2) US915 at the live SF9 default keeps its 60s cadence on the micro
 * profile (this fleet). Denser profiles legitimately stretch: with real
 * ToA, 60 SF9 beacons/hr (~18300 ms) exceed the baseline lane's entire
 * 18000 ms refill, which main never noticed because debits were both
 * fictional and unchecked. The adaptive policy itself already wants 120s
 * beacons above 10 neighbors (dense mode). */
void test_us915_default_cadence_unchanged_micro(void) {
    const bramble_freq_plan_t* us = freq_plan_get(FREQ_REGION_US915);
    const uint8_t micro_peers[] = {0, 5, 8};
    for (size_t p = 0; p < sizeof(micro_peers); p++) {
        gate_for(us, us->default_sf, micro_peers[p]);
        TEST_ASSERT_TRUE(tx_gate_min_beacon_interval_ms(&s_gate) <= CONFIGURED_INTERVAL);
    }
    /* Dense profile stretches, but stays well inside neighbor expiry. */
    gate_for(us, us->default_sf, 20);
    uint32_t dense_iv = tx_gate_min_beacon_interval_ms(&s_gate);
    TEST_ASSERT_TRUE(dense_iv > CONFIGURED_INTERVAL);
    TEST_ASSERT_TRUE(dense_iv < NEIGHBOR_EXPIRY / 2u);
}

/* (3) EU868 at the live SF9 default: the stretched cadence stays inside
 * neighbor expiry for every mesh-size profile (the liveness ceiling lets
 * beacons outrank broadcast data when the 60% share would not fit), so
 * duty enforcement cannot purge live neighbors. */
void test_eu868_sf9_cadence_within_neighbor_expiry(void) {
    const bramble_freq_plan_t* eu = freq_plan_get(FREQ_REGION_EU868);
    const uint8_t peers[] = {0, 5, 12, 20, 50};
    for (size_t p = 0; p < sizeof(peers); p++) {
        gate_for(eu, eu->default_sf, peers[p]);
        uint32_t min_iv = tx_gate_min_beacon_interval_ms(&s_gate);
        char msg[64];
        snprintf(msg, sizeof(msg), "peers=%u min_iv=%u", (unsigned)peers[p], (unsigned)min_iv);
        TEST_ASSERT_TRUE_MESSAGE(min_iv < NEIGHBOR_EXPIRY, msg);
    }
}

/* (4) Liveness under load: greedy broadcast data floods the lane for four
 * simulated hours; every beacon at the stretched cadence still transmits.
 * The one-beacon reserve plus the interval floor make this structural. */
void test_beacons_never_denied_under_broadcast_flood(void) {
    const bramble_freq_plan_t* eu = freq_plan_get(FREQ_REGION_EU868);
    gate_for(eu, eu->default_sf, 5); /* micro mesh, EU868 cap */

    uint32_t min_iv = tx_gate_min_beacon_interval_ms(&s_gate);
    TEST_ASSERT_TRUE(min_iv > CONFIGURED_INTERVAL); /* EU868 must stretch */

    uint8_t beacon[BEACON_WIRE_LEN] = {0};
    uint8_t data[200] = {0};
    uint32_t next_beacon = 0;
    int beacons_sent = 0;

    for (uint32_t t = 0; t <= 4u * AIRTIME_REFILL_INTERVAL_MS; t += 1000u) {
        s_fake.now = t;
        if (t >= next_beacon) {
            int rc = tx_gate_transmit(&s_gate, beacon, sizeof(beacon), TX_KIND_BEACON);
            char msg[64];
            snprintf(msg, sizeof(msg), "beacon denied at t=%u (n=%d)", (unsigned)t, beacons_sent);
            TEST_ASSERT_EQUAL_INT_MESSAGE(TX_GATE_OK, rc, msg);
            beacons_sent++;
            next_beacon = t + min_iv;
        }
        /* Greedy flood: broadcast data grabs every spendable token. */
        (void)tx_gate_transmit(&s_gate, data, sizeof(data), TX_KIND_DATA_BROADCAST);
    }
    TEST_ASSERT_TRUE(beacons_sent >= 40); /* ~49 beacons in 4h at ~290s */
}

/* The reserve only binds the BROADCAST lane: unicast data is unaffected. */
void test_reserve_does_not_touch_normal_lane(void) {
    const bramble_freq_plan_t* eu = freq_plan_get(FREQ_REGION_EU868);
    gate_for(eu, eu->default_sf, 5);
    uint8_t data[200] = {0};
    uint32_t before = airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_NORMAL);
    TEST_ASSERT_EQUAL_INT(TX_GATE_OK, tx_gate_transmit(&s_gate, data, sizeof(data), TX_KIND_DATA));
    TEST_ASSERT_TRUE(airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_NORMAL) < before);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_beacon_demand_fits_lane_at_every_sf_and_region);
    RUN_TEST(test_us915_default_cadence_unchanged_micro);
    RUN_TEST(test_eu868_sf9_cadence_within_neighbor_expiry);
    RUN_TEST(test_beacons_never_denied_under_broadcast_flood);
    RUN_TEST(test_reserve_does_not_touch_normal_lane);
    return UNITY_END();
}
