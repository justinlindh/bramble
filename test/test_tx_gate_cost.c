#include "unity.h"
#include <stdio.h>
#include "tx_gate.h"

/*
 * ToA cost-function tests (DES-7).
 *
 * Every expected value below is the exact output of the Semtech AN1200.13
 * formula as implemented in components/radio/radio_airtime.c:
 *   payloadSymbNb = 8 + max(ceil((8*PL - 4*SF + 28 + 16) / (4*(SF - 2*DE))) * (CR+4), 0)
 *   preamble 12 symbols for SF>=9 else 8, +4.25 sync; DE=1 for SF11/12 @125k.
 * tx_gate_cost_ms ceils microseconds to ms (never undercounts).
 */

void setUp(void) {}
void tearDown(void) {}

static uint8_t s_sf;
static uint32_t s_bw;
static uint8_t s_cr;

static void fake_toa_params(uint8_t* sf, uint32_t* bw_hz, uint8_t* cr) {
    *sf = s_sf;
    *bw_hz = s_bw;
    *cr = s_cr;
}
static bool fake_busy(void) { return false; }
static int fake_tx(const uint8_t* d, uint8_t l) {
    (void)d;
    (void)l;
    return 0;
}
static uint32_t fake_now(void) { return 0; }
static uint32_t fake_rand(void) { return 0; }
static void fake_delay(uint32_t ms) { (void)ms; }

static tx_gate_t s_gate;

static void gate_with(uint8_t sf, uint32_t bw, uint8_t cr) {
    s_sf = sf;
    s_bw = bw;
    s_cr = cr;
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

/* 44-byte packet at SF10/125k/CR1 (4/5): 567296 us -> 568 ms.
 * The old 30+len*4 heuristic said 206 ms: a 2.75x undercount. */
void test_44byte_sf10_125k_cr1(void) {
    gate_with(10, 125000, 1);
    TEST_ASSERT_EQUAL_UINT32(568u, tx_gate_cost_ms(&s_gate, 44));
}

/* Same packet at the live freq_plan default SF9: 304128 us -> 305 ms. */
void test_44byte_sf9_125k_cr1(void) {
    gate_with(9, 125000, 1);
    TEST_ASSERT_EQUAL_UINT32(305u, tx_gate_cost_ms(&s_gate, 44));
}

/* 22-byte ACK at SF10/125k/CR2 (4/6): 444416 us -> 445 ms.
 * Cross-checks the vector already pinned in test_radio_airtime.c. */
void test_22byte_ack_sf10_125k_cr2(void) {
    gate_with(10, 125000, 2);
    TEST_ASSERT_EQUAL_UINT32(445u, tx_gate_cost_ms(&s_gate, 22));
}

/* 12-byte routing control at SF9/125k/CR1: 160768 us -> 161 ms. */
void test_12byte_sf9_125k_cr1(void) {
    gate_with(9, 125000, 1);
    TEST_ASSERT_EQUAL_UINT32(161u, tx_gate_cost_ms(&s_gate, 12));
}

/* Max packet, 255 bytes at SF10/125k/CR1: 2328576 us -> 2329 ms. */
void test_255byte_sf10_125k_cr1(void) {
    gate_with(10, 125000, 1);
    TEST_ASSERT_EQUAL_UINT32(2329u, tx_gate_cost_ms(&s_gate, 255));
}

/* SF12 with low-data-rate optimization: 44 bytes -> 2269184 us -> 2270 ms.
 * The heuristic said 206 ms: an 11x undercount at SF12. */
void test_44byte_sf12_125k_cr1(void) {
    gate_with(12, 125000, 1);
    TEST_ASSERT_EQUAL_UINT32(2270u, tx_gate_cost_ms(&s_gate, 44));
}

/* Medium-range profile: 100 bytes at SF7/250k/CR1: 87168 us -> 88 ms. */
void test_100byte_sf7_250k_cr1(void) {
    gate_with(7, 250000, 1);
    TEST_ASSERT_EQUAL_UINT32(88u, tx_gate_cost_ms(&s_gate, 100));
}

/* Unconfigured radio (all zeros) falls back to SF9/125k/CR1: a 17-byte
 * packet costs 181248 us -> 182 ms, not a division by zero. */
void test_unconfigured_radio_falls_back(void) {
    gate_with(0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(182u, tx_gate_cost_ms(&s_gate, 17));
}

/* The cost the gate debits must match the cost it checks: one source of
 * truth, exercised end to end through tx_gate_transmit. */
void test_transmit_debits_exact_toa(void) {
    gate_with(10, 125000, 1);
    uint8_t pkt[44] = {0};
    uint32_t before = airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_NORMAL);
    TEST_ASSERT_EQUAL_INT(TX_GATE_OK, tx_gate_transmit(&s_gate, pkt, sizeof(pkt), TX_KIND_DATA));
    uint32_t after = airtime_budget_remaining(&s_gate.budget, AIRTIME_TIER_NORMAL);
    TEST_ASSERT_EQUAL_UINT32(568u, before - after);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_44byte_sf10_125k_cr1);
    RUN_TEST(test_44byte_sf9_125k_cr1);
    RUN_TEST(test_22byte_ack_sf10_125k_cr2);
    RUN_TEST(test_12byte_sf9_125k_cr1);
    RUN_TEST(test_255byte_sf10_125k_cr1);
    RUN_TEST(test_44byte_sf12_125k_cr1);
    RUN_TEST(test_100byte_sf7_250k_cr1);
    RUN_TEST(test_unconfigured_radio_falls_back);
    RUN_TEST(test_transmit_debits_exact_toa);
    return UNITY_END();
}
