#include "unity.h"
#include "esp_stubs.h"
#include "packet.h"
#include "../components/packet/packet.c"

void setUp(void) {}
void tearDown(void) {}

void test_current_version_is_two(void) {
    TEST_ASSERT_EQUAL_UINT8(2, BRAMBLE_VERSION);
}

void test_supported_version_accepts_current(void) {
    bramble_header_t h = {.version = BRAMBLE_VERSION, .type = PKT_TYPE_DATA};
    TEST_ASSERT_TRUE(bramble_header_is_supported_version(&h));
}

void test_supported_version_rejects_v1(void) {
    bramble_header_t h = {.version = 1, .type = PKT_TYPE_DATA};
    TEST_ASSERT_FALSE(bramble_header_is_supported_version(&h));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_current_version_is_two);
    RUN_TEST(test_supported_version_accepts_current);
    RUN_TEST(test_supported_version_rejects_v1);
    return UNITY_END();
}
