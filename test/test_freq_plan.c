#include "unity.h"
#include "freq_plan.h"

void setUp(void) {}
void tearDown(void) {}

void test_region_boundaries_are_inclusive(void) {
    const bramble_freq_plan_t *eu = freq_plan_get(FREQ_REGION_EU868);
    TEST_ASSERT_NOT_NULL(eu);

    TEST_ASSERT_TRUE(freq_plan_valid_freq(eu, eu->freq_start_mhz));
    TEST_ASSERT_TRUE(freq_plan_valid_freq(eu, eu->freq_end_mhz));
    TEST_ASSERT_FALSE(freq_plan_valid_freq(eu, eu->freq_start_mhz - 0.001f));
    TEST_ASSERT_FALSE(freq_plan_valid_freq(eu, eu->freq_end_mhz + 0.001f));
}

void test_power_validation_and_clamp(void) {
    const bramble_freq_plan_t *eu = freq_plan_get(FREQ_REGION_EU868);
    TEST_ASSERT_NOT_NULL(eu);

    TEST_ASSERT_TRUE(freq_plan_valid_power(eu, eu->max_tx_power_dbm));
    TEST_ASSERT_FALSE(freq_plan_valid_power(eu, eu->max_tx_power_dbm + 1));

    TEST_ASSERT_EQUAL_INT8(eu->max_tx_power_dbm,
                           freq_plan_clamp_power(eu, eu->max_tx_power_dbm + 20));
    TEST_ASSERT_EQUAL_INT8(10, freq_plan_clamp_power(eu, 10));
}

void test_invalid_region_and_null_plan_guards(void) {
    TEST_ASSERT_NULL(freq_plan_get(FREQ_REGION_COUNT));
    TEST_ASSERT_FALSE(freq_plan_valid_freq(NULL, 915.0f));
    TEST_ASSERT_FALSE(freq_plan_valid_power(NULL, 20));
    TEST_ASSERT_EQUAL_INT8(0, freq_plan_clamp_power(NULL, 20));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_region_boundaries_are_inclusive);
    RUN_TEST(test_power_validation_and_clamp);
    RUN_TEST(test_invalid_region_and_null_plan_guards);
    return UNITY_END();
}
