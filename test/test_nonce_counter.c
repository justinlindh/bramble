#include "unity.h"
#include "nonce_counter.h"
#include "../components/nonce_counter/nonce_counter.c"
#include <string.h>

static uint64_t g_flash;      /* mock NVS ceiling */
static int g_writes;
static int mock_read(uint64_t* out, void* ctx) { (void)ctx; *out = g_flash; return 0; }
static int mock_write(uint64_t v, void* ctx) { (void)ctx; g_flash = v; g_writes++; return 0; }

void setUp(void) { g_flash = 0; g_writes = 0; }
void tearDown(void) {}

void test_nonce_layout(void) {
    nonce_counter_init(0xA1B2C3D4u, 0xBEEF, mock_read, mock_write, NULL);
    uint8_t n[12];
    TEST_ASSERT_EQUAL(0, nonce_counter_next(n));
    TEST_ASSERT_EQUAL_HEX8(0xA1, n[0]); TEST_ASSERT_EQUAL_HEX8(0xB2, n[1]);
    TEST_ASSERT_EQUAL_HEX8(0xC3, n[2]); TEST_ASSERT_EQUAL_HEX8(0xD4, n[3]);
    TEST_ASSERT_EQUAL_HEX8(0xBE, n[4]); TEST_ASSERT_EQUAL_HEX8(0xEF, n[5]);
    /* first counter is 0 (ceiling was 0, reserve-ahead issues from prior ceiling) */
    TEST_ASSERT_EQUAL_UINT64(0, nonce_counter_extract(n));
}

void test_monotonic_and_reserve_ahead(void) {
    nonce_counter_init(1, 0, mock_read, mock_write, NULL);
    /* init reserves one block ahead */
    TEST_ASSERT_EQUAL(1, g_writes);
    TEST_ASSERT_EQUAL_UINT64(NONCE_RESERVE, g_flash);
    uint8_t n[12];
    uint64_t prev = 0;
    for (int i = 0; i < 5; i++) {
        nonce_counter_next(n);
        uint64_t c = nonce_counter_extract(n);
        if (i > 0) TEST_ASSERT_TRUE(c > prev);
        prev = c;
    }
}

void test_reboot_resumes_above_ceiling(void) {
    nonce_counter_init(1, 0, mock_read, mock_write, NULL);   /* flash -> 65536 */
    uint8_t n[12];
    for (int i = 0; i < 3; i++) nonce_counter_next(n);
    /* simulate reboot: re-init reads the persisted ceiling */
    nonce_counter_init(1, 0, mock_read, mock_write, NULL);
    nonce_counter_next(n);
    TEST_ASSERT_TRUE(nonce_counter_extract(n) >= NONCE_RESERVE);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_nonce_layout);
    RUN_TEST(test_monotonic_and_reserve_ahead);
    RUN_TEST(test_reboot_resumes_above_ceiling);
    return UNITY_END();
}
