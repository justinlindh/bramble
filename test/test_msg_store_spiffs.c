/*
 * Test suite for msg_store SPIFFS persistence module
 *
 * Note: These tests run on host (non-ESP), so SPIFFS functions are stubbed.
 * Full integration tests should be run on actual hardware with SPIFFS mounted.
 */

#include "unity.h"
#include "msg_store.h"
#include "msg_store_spiffs.h"

void setUp(void) { msg_store_init(); }

void tearDown(void) {}

/* Host tests verify API contracts, not actual SPIFFS behavior */

void test_spiffs_init_returns_error_on_host(void) {
    /* On host (no ESP_PLATFORM), SPIFFS functions are stubbed to return errors */
    int ret = msg_store_spiffs_init();
    TEST_ASSERT_EQUAL(-1, ret);
}

void test_spiffs_save_returns_error_when_not_initialized(void) {
    stored_msg_t msg = {0};
    int ret = msg_store_spiffs_save(&msg);
    TEST_ASSERT_EQUAL(-1, ret);
}

void test_spiffs_get_count_returns_zero_on_host(void) {
    int count = msg_store_spiffs_get_count();
    TEST_ASSERT_EQUAL(0, count);
}

void test_spiffs_load_recent_returns_zero_on_host(void) {
    stored_msg_t msgs[10];
    int loaded = msg_store_spiffs_load_recent(msgs, 10);
    TEST_ASSERT_EQUAL(0, loaded);
}

void test_spiffs_clear_does_not_crash(void) {
    /* Just verify it doesn't crash on host */
    msg_store_spiffs_clear();
    TEST_PASS();
}

void test_spiffs_rollover_does_not_crash(void) {
    /* Just verify it doesn't crash on host */
    msg_store_spiffs_rollover(100, 75);
    TEST_PASS();
}

/* Integration test: msg_store_init_with_persistence */

void test_msg_store_init_with_persistence_works_without_spiffs(void) {
    /* Should initialize normally even when SPIFFS is not available */
    msg_store_init_with_persistence();
    TEST_ASSERT_EQUAL(0, msg_store_count());
}

void test_msg_store_init_with_persistence_preserves_ram_functionality(void) {
    msg_store_init_with_persistence();

    /* Add a message to RAM */
    msg_store_add_ex2(0x1234, MSG_DIR_OUTGOING, "test", 4, -70, 5, 1, MSG_STATUS_SENT, 0);

    /* Verify it's in RAM */
    TEST_ASSERT_EQUAL(1, msg_store_count());
    const stored_msg_t* m = msg_store_get(0);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL(0x1234, m->peer_addr);
    TEST_ASSERT_EQUAL_STRING("test", m->text);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_spiffs_init_returns_error_on_host);
    RUN_TEST(test_spiffs_save_returns_error_when_not_initialized);
    RUN_TEST(test_spiffs_get_count_returns_zero_on_host);
    RUN_TEST(test_spiffs_load_recent_returns_zero_on_host);
    RUN_TEST(test_spiffs_clear_does_not_crash);
    RUN_TEST(test_spiffs_rollover_does_not_crash);
    RUN_TEST(test_msg_store_init_with_persistence_works_without_spiffs);
    RUN_TEST(test_msg_store_init_with_persistence_preserves_ram_functionality);
    return UNITY_END();
}
