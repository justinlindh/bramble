#include "unity.h"
#include "nonce_counter.h"
#include "../components/nonce_counter/nonce_counter.c"
#include <string.h>

static uint64_t g_flash; /* mock NVS ceiling */
static int g_writes;
static int g_write_should_fail; /* simulates a transient/worn-flash write failure */
static int mock_read(uint64_t* out, void* ctx) {
    (void)ctx;
    *out = g_flash;
    return 0;
}
static int mock_write(uint64_t v, void* ctx) {
    (void)ctx;
    if (g_write_should_fail)
        return -1; /* not durable: g_flash must NOT change */
    g_flash = v;
    g_writes++;
    return 0;
}

void setUp(void) {
    g_flash = 0;
    g_writes = 0;
    g_write_should_fail = 0;
}
void tearDown(void) {}

void test_nonce_layout(void) {
    TEST_ASSERT_EQUAL(0, nonce_counter_init(0xA1B2C3D4u, 0xBEEF, mock_read, mock_write, NULL));
    uint8_t n[12];
    TEST_ASSERT_EQUAL(0, nonce_counter_next(n));
    TEST_ASSERT_EQUAL_HEX8(0xA1, n[0]);
    TEST_ASSERT_EQUAL_HEX8(0xB2, n[1]);
    TEST_ASSERT_EQUAL_HEX8(0xC3, n[2]);
    TEST_ASSERT_EQUAL_HEX8(0xD4, n[3]);
    TEST_ASSERT_EQUAL_HEX8(0xBE, n[4]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, n[5]);
    /* first counter is 0 (ceiling was 0, reserve-ahead issues from prior ceiling) */
    TEST_ASSERT_EQUAL_UINT64(0, nonce_counter_extract(n));
}

void test_monotonic_and_reserve_ahead(void) {
    TEST_ASSERT_EQUAL(0, nonce_counter_init(1, 0, mock_read, mock_write, NULL));
    /* init reserves one block ahead */
    TEST_ASSERT_EQUAL(1, g_writes);
    TEST_ASSERT_EQUAL_UINT64(NONCE_RESERVE, g_flash);
    uint8_t n[12];
    uint64_t prev = 0;
    for (int i = 0; i < 5; i++) {
        nonce_counter_next(n);
        uint64_t c = nonce_counter_extract(n);
        if (i > 0)
            TEST_ASSERT_TRUE(c > prev);
        prev = c;
    }
}

void test_reboot_resumes_above_ceiling(void) {
    TEST_ASSERT_EQUAL(0,
                      nonce_counter_init(1, 0, mock_read, mock_write, NULL)); /* flash -> 65536 */
    uint8_t n[12];
    for (int i = 0; i < 3; i++)
        nonce_counter_next(n);
    /* simulate reboot: re-init reads the persisted ceiling */
    TEST_ASSERT_EQUAL(0, nonce_counter_init(1, 0, mock_read, mock_write, NULL));
    nonce_counter_next(n);
    TEST_ASSERT_TRUE(nonce_counter_extract(n) >= NONCE_RESERVE);
}

/* CRITICAL 1 fix: init must fail closed, not silently issue nonces the
 * persisted ceiling never actually covers. */
void test_init_fails_closed_when_reserve_write_fails(void) {
    g_write_should_fail = 1;
    TEST_ASSERT_EQUAL(-1, nonce_counter_init(1, 0, mock_read, mock_write, NULL));
    TEST_ASSERT_EQUAL(0, g_writes); /* nothing was ever confirmed durable */

    uint8_t n[12];
    /* Subsystem is unusable until a write succeeds: next() must refuse too. */
    TEST_ASSERT_NOT_EQUAL(0, nonce_counter_next(n));
}

/* CRITICAL 1 fix: a boundary-crossing flush that fails must issue nothing
 * and must not advance the counter, so a retry (once writes recover) issues
 * the exact same next value rather than skipping or reusing one. */
void test_boundary_flush_failure_blocks_issuance_then_resumes(void) {
    TEST_ASSERT_EQUAL(0,
                      nonce_counter_init(1, 0, mock_read, mock_write, NULL)); /* flash -> 65536 */

    uint8_t n[12];
    for (uint64_t i = 0; i < NONCE_RESERVE; i++) {
        TEST_ASSERT_EQUAL(0, nonce_counter_next(n));
        TEST_ASSERT_EQUAL_UINT64(i, nonce_counter_extract(n));
    }

    /* The next call crosses the reserved block boundary and must flush a new
     * ceiling; make that write fail. */
    g_write_should_fail = 1;
    TEST_ASSERT_NOT_EQUAL(0, nonce_counter_next(n));
    /* Retrying while still failing must not have silently advanced state. */
    TEST_ASSERT_NOT_EQUAL(0, nonce_counter_next(n));
    TEST_ASSERT_EQUAL_UINT64(NONCE_RESERVE, g_flash); /* still the OLD durable ceiling */

    /* Once the write path recovers, the exact next counter value is issued. */
    g_write_should_fail = 0;
    TEST_ASSERT_EQUAL(0, nonce_counter_next(n));
    TEST_ASSERT_EQUAL_UINT64(NONCE_RESERVE, nonce_counter_extract(n));
    TEST_ASSERT_EQUAL_UINT64(2u * NONCE_RESERVE, g_flash); /* new ceiling now durable */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_nonce_layout);
    RUN_TEST(test_monotonic_and_reserve_ahead);
    RUN_TEST(test_reboot_resumes_above_ceiling);
    RUN_TEST(test_init_fails_closed_when_reserve_write_fails);
    RUN_TEST(test_boundary_flush_failure_blocks_issuance_then_resumes);
    return UNITY_END();
}
