#include "unity/unity.h"
#include "route_metric.h"

void setUp(void) {}
void tearDown(void) {}

void test_metric_compute_balanced(void)
{
    uint8_t m = route_metric_compute(128, 128, 128, 512);
    /* latency_score = 255 - 128 = 127; weighted avg of ~128,128,128,127 ≈ 128 */
    TEST_ASSERT_UINT8_WITHIN(3, 128, m);
}

void test_metric_compute_link_dominant(void)
{
    uint8_t high = route_metric_compute(255, 50, 50, 800);
    uint8_t low  = route_metric_compute(50, 50, 50, 800);
    TEST_ASSERT_TRUE(high > low);
    /* Link weight is 40%, so difference should be significant */
    TEST_ASSERT_TRUE(high - low > 50);
}

void test_metric_compute_zero_delivery(void)
{
    uint8_t m = route_metric_compute(200, 0, 200, 100);
    /* Without delivery (30% weight), metric should be noticeably lower */
    uint8_t full = route_metric_compute(200, 200, 200, 100);
    TEST_ASSERT_TRUE(full - m > 40);
}

void test_metric_delivery_ema_converges(void)
{
    uint8_t rate = 0;
    for (int i = 0; i < 8; i++)
        rate = route_metric_update_delivery(rate, true);
    /* After 8 successes from 0, should approach 255 */
    TEST_ASSERT_TRUE(rate > 150);
}

void test_metric_delivery_ema_failure(void)
{
    uint8_t rate = 128;
    /* Mix: success, fail, success, fail ... */
    for (int i = 0; i < 20; i++)
        rate = route_metric_update_delivery(rate, i % 2 == 0);
    /* Should settle around ~half */
    TEST_ASSERT_UINT8_WITHIN(40, 128, rate);
}

void test_metric_latency_ema(void)
{
    uint16_t avg = 100;
    avg = route_metric_update_latency(avg, 500);
    /* EMA 1/4: 100 - 25 + 125 = 200 */
    TEST_ASSERT_EQUAL_UINT16(200, avg);
    avg = route_metric_update_latency(avg, 200);
    /* 200 - 50 + 50 = 200 */
    TEST_ASSERT_EQUAL_UINT16(200, avg);
}

void test_metric_should_switch_hysteresis(void)
{
    /* Small improvement: rejected */
    TEST_ASSERT_FALSE(route_metric_should_switch(100, 110, 0, 20000));
    /* Large improvement: accepted */
    TEST_ASSERT_TRUE(route_metric_should_switch(100, 120, 0, 20000));
}

void test_metric_should_switch_cooldown(void)
{
    /* Good improvement but during cooldown: rejected */
    TEST_ASSERT_FALSE(route_metric_should_switch(100, 200, 15000, 20000));
    /* Same improvement after cooldown: accepted */
    TEST_ASSERT_TRUE(route_metric_should_switch(100, 200, 5000, 20000));
}

void test_metric_airtime_score(void)
{
    TEST_ASSERT_EQUAL_UINT8(255, route_metric_airtime_score(1000, 1000));
    TEST_ASSERT_EQUAL_UINT8(0, route_metric_airtime_score(0, 1000));
    uint8_t half = route_metric_airtime_score(500, 1000);
    TEST_ASSERT_UINT8_WITHIN(2, 127, half);
    /* Edge case: max_ms = 0 */
    TEST_ASSERT_EQUAL_UINT8(0, route_metric_airtime_score(500, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_metric_compute_balanced);
    RUN_TEST(test_metric_compute_link_dominant);
    RUN_TEST(test_metric_compute_zero_delivery);
    RUN_TEST(test_metric_delivery_ema_converges);
    RUN_TEST(test_metric_delivery_ema_failure);
    RUN_TEST(test_metric_latency_ema);
    RUN_TEST(test_metric_should_switch_hysteresis);
    RUN_TEST(test_metric_should_switch_cooldown);
    RUN_TEST(test_metric_airtime_score);
    return UNITY_END();
}
