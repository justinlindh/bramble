#include "unity.h"
#include "esp_stubs.h"
#include "packet.h"
#include "../components/packet/packet.c"

void setUp(void) {}
void tearDown(void) {}

void test_aead_aad_appends_src_and_masks_hoplimit(void) {
    bramble_header_t h = {.version = BRAMBLE_VERSION,
                          .type = PKT_TYPE_DATA,
                          .flags = FLAG_ENCRYPT,
                          .hop_limit = 7,
                          .dest_addr = 0x11223344,
                          .packet_id = 0xAABBCCDD};
    uint8_t aad[HEADER_SIZE + 4];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_build_aead_aad(&h, 0xA1B2C3D4u, aad, sizeof(aad)));
    /* hop_limit byte (index 3) masked to 0 */
    TEST_ASSERT_EQUAL_HEX8(0, aad[3]);
    /* src_addr little-endian in the last 4 bytes */
    TEST_ASSERT_EQUAL_HEX8(0xD4, aad[HEADER_SIZE + 0]);
    TEST_ASSERT_EQUAL_HEX8(0xC3, aad[HEADER_SIZE + 1]);
    TEST_ASSERT_EQUAL_HEX8(0xB2, aad[HEADER_SIZE + 2]);
    TEST_ASSERT_EQUAL_HEX8(0xA1, aad[HEADER_SIZE + 3]);
}

void test_aead_aad_rejects_short_buffer(void) {
    bramble_header_t h = {.version = BRAMBLE_VERSION};
    uint8_t small[HEADER_SIZE];
    TEST_ASSERT_NOT_EQUAL(ESP_OK, bramble_build_aead_aad(&h, 0, small, sizeof(small)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_aead_aad_appends_src_and_masks_hoplimit);
    RUN_TEST(test_aead_aad_rejects_short_buffer);
    return UNITY_END();
}
