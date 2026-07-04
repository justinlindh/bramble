#include "unity.h"
#include <string.h>
#include "probe_results.h"

void setUp(void) {}
void tearDown(void) {}

void test_insert_new_responder(void) {
    probe_result_t r[MAX_PROBE_RESULTS];
    int count = 0;
    memset(r, 0, sizeof(r));
    probe_results_upsert(r, &count, MAX_PROBE_RESULTS, 0xAAAA, 2, -80, 5, 1234u, 1);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_UINT32(0xAAAAu, r[0].addr);
    TEST_ASSERT_EQUAL_UINT8(2, r[0].hops);
    TEST_ASSERT_EQUAL_INT16(-80, r[0].rssi);
    TEST_ASSERT_EQUAL_UINT8(0x01, r[0].seen_round_mask);
}

void test_upsert_keeps_best_rssi_snr_latest_latency(void) {
    probe_result_t r[MAX_PROBE_RESULTS];
    int count = 0;
    memset(r, 0, sizeof(r));
    probe_results_upsert(r, &count, MAX_PROBE_RESULTS, 0xAAAA, 2, -80, 5, 1000u, 1);
    probe_results_upsert(r, &count, MAX_PROBE_RESULTS, 0xAAAA, 3, -70, 9, 2000u, 2);
    probe_results_upsert(r, &count, MAX_PROBE_RESULTS, 0xAAAA, 4, -90, 2, 3000u, 3);
    TEST_ASSERT_EQUAL_INT(1, count);                     /* still one logical row */
    TEST_ASSERT_EQUAL_INT16(-70, r[0].rssi);             /* best (max) RSSI */
    TEST_ASSERT_EQUAL_INT8(9, r[0].snr);                 /* best (max) SNR */
    TEST_ASSERT_EQUAL_UINT32(3000u, r[0].latency_ms);    /* latest */
    TEST_ASSERT_EQUAL_UINT8(4, r[0].hops);               /* latest hops */
    TEST_ASSERT_EQUAL_UINT8(0x07, r[0].seen_round_mask); /* rounds 1|2|3 */
}

void test_second_responder_added(void) {
    probe_result_t r[MAX_PROBE_RESULTS];
    int count = 0;
    memset(r, 0, sizeof(r));
    probe_results_upsert(r, &count, MAX_PROBE_RESULTS, 0xAAAA, 1, -60, 8, 100u, 1);
    probe_results_upsert(r, &count, MAX_PROBE_RESULTS, 0xBBBB, 1, -61, 7, 110u, 1);
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_UINT32(0xBBBBu, r[1].addr);
}

void test_capacity_ceiling_drops_overflow_but_updates_existing(void) {
    probe_result_t r[MAX_PROBE_RESULTS];
    int count = 0;
    memset(r, 0, sizeof(r));
    for (int i = 0; i < MAX_PROBE_RESULTS; i++)
        probe_results_upsert(r, &count, MAX_PROBE_RESULTS, 0x1000u + (uint32_t)i, 1, -70, 5, 10u,
                             1);
    TEST_ASSERT_EQUAL_INT(MAX_PROBE_RESULTS, count);
    /* Overflow insert of a NEW addr is dropped. */
    probe_results_upsert(r, &count, MAX_PROBE_RESULTS, 0xFFFFu, 1, -70, 5, 10u, 1);
    TEST_ASSERT_EQUAL_INT(MAX_PROBE_RESULTS, count);
    /* Update of an EXISTING addr still applies. */
    probe_results_upsert(r, &count, MAX_PROBE_RESULTS, 0x1000u, 2, -50, 9, 99u, 2);
    TEST_ASSERT_EQUAL_INT16(-50, r[0].rssi);
    TEST_ASSERT_EQUAL_UINT8(0x03, r[0].seen_round_mask);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_insert_new_responder);
    RUN_TEST(test_upsert_keeps_best_rssi_snr_latest_latency);
    RUN_TEST(test_second_responder_added);
    RUN_TEST(test_capacity_ceiling_drops_overflow_but_updates_existing);
    return UNITY_END();
}
