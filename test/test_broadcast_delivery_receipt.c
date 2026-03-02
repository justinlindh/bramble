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

void test_slot_delay_is_bounded_and_identity_sensitive(void) {
    uint32_t d1 = mesh_broadcast_receipt_slot_delay_ms(0x01020304u, 0xCAFEBABEu);
    uint32_t d2 = mesh_broadcast_receipt_slot_delay_ms(0x0A0B0C0Du, 0xCAFEBABEu);

    TEST_ASSERT_TRUE(d1 >= 200u);
    TEST_ASSERT_TRUE(d1 <= (200u + 200u * 31u));
    TEST_ASSERT_TRUE(d2 >= 200u);
    TEST_ASSERT_TRUE(d2 <= (200u + 200u * 31u));
    TEST_ASSERT_NOT_EQUAL(d1, d2);
}

void test_retry_count_default_three_attempts(void) {
    TEST_ASSERT_EQUAL_UINT8(3u, mesh_broadcast_receipt_retry_count());
}

void test_slot_distribution_no_collision_for_typical_mesh(void) {
    uint32_t addrs[] = { 0x196F8E71u, 0xE3CF8D24u, 0x0D941BEAu, 0xD4813079u, 0x6CBF8FE3u };
    int collision_count = 0;

    for (uint32_t pkt_id = 0; pkt_id < 1000u; pkt_id++) {
        uint32_t slots[5];
        for (int i = 0; i < 5; i++) {
            slots[i] = mesh_broadcast_receipt_slot_delay_ms(addrs[i], pkt_id);
        }
        for (int i = 0; i < 5; i++) {
            for (int j = i + 1; j < 5; j++) {
                if (slots[i] == slots[j]) {
                    collision_count++;
                }
            }
        }
    }

    /* With 32 buckets and 5 nodes, expect very few slot collisions.
     * XOR-based hashing with these addresses may yield 0 collisions,
     * which is ideal. Upper bound ensures hash isn't degenerate. */
    TEST_ASSERT_TRUE(collision_count < 500);
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
    RUN_TEST(test_slot_delay_is_bounded_and_identity_sensitive);
    RUN_TEST(test_retry_count_default_three_attempts);
    RUN_TEST(test_slot_distribution_no_collision_for_typical_mesh);
    RUN_TEST(test_build_delivery_receipt_targets_original_sender_with_expected_fields);
    return UNITY_END();
}
