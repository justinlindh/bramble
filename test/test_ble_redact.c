#include "unity.h"
#include "ble_redact.h"

void setUp(void) {}
void tearDown(void) {}

void test_preauth_body_never_loggable(void) {
    /* The first pre-auth write is the bare token; it must never be echoed. */
    TEST_ASSERT_FALSE(ble_rpc_body_loggable(false));
}

void test_postauth_body_loggable(void) { TEST_ASSERT_TRUE(ble_rpc_body_loggable(true)); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_preauth_body_never_loggable);
    RUN_TEST(test_postauth_body_loggable);
    return UNITY_END();
}
