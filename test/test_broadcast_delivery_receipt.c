#include "unity.h"

#include "broadcast_delivery_receipt.h"
#include "packet.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_should_emit_only_for_broadcast_dest(void) {
    TEST_ASSERT_TRUE(mesh_should_emit_broadcast_delivery_receipt(0xFFFFFFFFu));
    TEST_ASSERT_FALSE(mesh_should_emit_broadcast_delivery_receipt(0x01020304u));
}

void test_build_delivery_receipt_targets_original_sender_with_expected_fields(void) {
    uint8_t buf[DELIVERY_RECEIPT_MAX_SIZE];
    size_t wire_len = 0;

    esp_err_t err = mesh_build_broadcast_delivery_receipt_packet(0xAABBCCDDu,
                                                                 0x11223344u,
                                                                 0x55667788u,
                                                                 0xCAFEBABEu,
                                                                 buf,
                                                                 sizeof(buf),
                                                                 &wire_len);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(DELIVERY_RECEIPT_MIN_SIZE + 4, wire_len);

    bramble_delivery_receipt_t decoded;
    err = bramble_delivery_receipt_deserialize(&decoded, buf, wire_len);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    TEST_ASSERT_EQUAL(PKT_TYPE_DELIVERY_RECEIPT, decoded.header.type);
    TEST_ASSERT_EQUAL(0x55667788u, decoded.header.dest_addr);
    TEST_ASSERT_EQUAL(0x11223344u, decoded.header.packet_id);
    TEST_ASSERT_EQUAL(0xAABBCCDDu, decoded.src_addr);
    TEST_ASSERT_EQUAL(0xCAFEBABEu, decoded.orig_packet_id);
    TEST_ASSERT_EQUAL_UINT8(1, decoded.hop_count);
    TEST_ASSERT_EQUAL(0xAABBCCDDu, decoded.relay_path[0]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_should_emit_only_for_broadcast_dest);
    RUN_TEST(test_build_delivery_receipt_targets_original_sender_with_expected_fields);
    return UNITY_END();
}
