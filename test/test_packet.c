#include "unity.h"
#include "esp_stubs.h"
#include "packet.h"
#include "../components/location/include/location.h"
#include "packet.c"

void setUp(void) {}
void tearDown(void) {}

/* Helper: fill a standard header */
static bramble_header_t make_header(uint8_t type) {
    bramble_header_t h = {
        .version   = BRAMBLE_VERSION,
        .type      = type,
        .flags     = FLAG_ACK_REQ | FLAG_ENCRYPT,
        .hop_limit = 7,
        .dest_addr = 0xDEADBEEF,
        .packet_id = 0x12345678,
    };
    return h;
}

/* ---- Header tests ---- */
void test_header_roundtrip(void) {
    bramble_header_t h = make_header(PKT_TYPE_DATA);
    uint8_t buf[HEADER_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_serialize(&h, buf, sizeof(buf)));
    bramble_header_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(h.version, out.version);
    TEST_ASSERT_EQUAL(h.type, out.type);
    TEST_ASSERT_EQUAL(h.flags, out.flags);
    TEST_ASSERT_EQUAL(h.hop_limit, out.hop_limit);
    TEST_ASSERT_EQUAL_HEX32(h.dest_addr, out.dest_addr);
    TEST_ASSERT_EQUAL_HEX32(h.packet_id, out.packet_id);
}

void test_header_buffer_too_small(void) {
    bramble_header_t h = make_header(PKT_TYPE_DATA);
    uint8_t buf[HEADER_SIZE - 1];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, bramble_header_serialize(&h, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, bramble_header_deserialize(&h, buf, sizeof(buf)));
}

void test_header_big_endian_wire(void) {
    bramble_header_t h = {
        .version = 1, .type = 0x0A, .flags = 0x24, .hop_limit = 7,
        .dest_addr = 0xDEADBEEF, .packet_id = 0x12345678,
    };
    uint8_t buf[HEADER_SIZE];
    bramble_header_serialize(&h, buf, sizeof(buf));
    /* dest_addr at offset 4: DE AD BE EF */
    TEST_ASSERT_EQUAL_HEX8(0xDE, buf[4]);
    TEST_ASSERT_EQUAL_HEX8(0xAD, buf[5]);
    TEST_ASSERT_EQUAL_HEX8(0xBE, buf[6]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, buf[7]);
    /* packet_id at offset 8: 12 34 56 78 */
    TEST_ASSERT_EQUAL_HEX8(0x12, buf[8]);
    TEST_ASSERT_EQUAL_HEX8(0x34, buf[9]);
    TEST_ASSERT_EQUAL_HEX8(0x56, buf[10]);
    TEST_ASSERT_EQUAL_HEX8(0x78, buf[11]);
}

/* ---- ACK ---- */
void test_ack_roundtrip(void) {
    bramble_ack_t p = {
        .header = make_header(PKT_TYPE_ACK),
        .src_addr = 0xAABBCCDD, .ack_packet_id = 0x11223344,
        .ack_flags = 0x03, .rssi_at_dest = -72,
    };
    uint8_t buf[ACK_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&p, buf, sizeof(buf)));
    bramble_ack_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX32(p.src_addr, out.src_addr);
    TEST_ASSERT_EQUAL_HEX32(p.ack_packet_id, out.ack_packet_id);
    TEST_ASSERT_EQUAL(p.ack_flags, out.ack_flags);
    TEST_ASSERT_EQUAL(p.rssi_at_dest, out.rssi_at_dest);
}

void test_ack_buffer_too_small(void) {
    bramble_ack_t p = { .header = make_header(PKT_TYPE_ACK) };
    /* Full-size backing array, undersized length: same rejection contract,
     * but GCC's -Warray-bounds path analysis cannot flag the (unreachable)
     * serializer writes past a too-small array. */
    uint8_t buf[ACK_SIZE] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, bramble_ack_serialize(&p, buf, sizeof(buf) - 1));
}

/* ---- RREQ ---- */
void test_rreq_roundtrip(void) {
    bramble_rreq_t p = {
        .header = make_header(PKT_TYPE_RREQ),
        .query_id = 0x99887766, .encrypted_source = 0x55443322,
        .hop_count = 3, .metric = 42, .prev_hop = 0xFEDCBA98,
        .rreq_salt = 0xABCD1234,
    };
    uint8_t buf[RREQ_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rreq_serialize(&p, buf, sizeof(buf)));
    bramble_rreq_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rreq_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX32(p.query_id, out.query_id);
    TEST_ASSERT_EQUAL_HEX32(p.encrypted_source, out.encrypted_source);
    TEST_ASSERT_EQUAL(p.hop_count, out.hop_count);
    TEST_ASSERT_EQUAL(p.metric, out.metric);
    TEST_ASSERT_EQUAL_HEX32(p.prev_hop, out.prev_hop);
    TEST_ASSERT_EQUAL_HEX32(p.rreq_salt, out.rreq_salt);
}

/* ---- RREP ---- */
void test_rrep_roundtrip(void) {
    bramble_rrep_t p = {
        .header = make_header(PKT_TYPE_RREP),
        .query_id = 0xAAAAAAAA, .src_addr = 0xBBBBBBBB,
        .next_hop = 0xCCCCCCCC, .hop_count = 5, .route_metric = 100,
        .auth_hmac = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE},
    };
    uint8_t buf[RREP_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rrep_serialize(&p, buf, sizeof(buf)));
    bramble_rrep_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rrep_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX32(p.query_id, out.query_id);
    TEST_ASSERT_EQUAL_HEX32(p.src_addr, out.src_addr);
    TEST_ASSERT_EQUAL_HEX32(p.next_hop, out.next_hop);
    TEST_ASSERT_EQUAL(p.hop_count, out.hop_count);
    TEST_ASSERT_EQUAL(p.route_metric, out.route_metric);
    TEST_ASSERT_EQUAL_MEMORY(p.auth_hmac, out.auth_hmac, 8);
}

/* ---- RERR ---- */
void test_rerr_roundtrip(void) {
    bramble_rerr_t p = {
        .header = make_header(PKT_TYPE_RERR),
        .reporter_addr = 0x11111111, .broken_dest = 0x22222222,
        .broken_next_hop = 0x33333333,
    };
    uint8_t buf[RERR_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rerr_serialize(&p, buf, sizeof(buf)));
    bramble_rerr_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rerr_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX32(p.reporter_addr, out.reporter_addr);
    TEST_ASSERT_EQUAL_HEX32(p.broken_dest, out.broken_dest);
    TEST_ASSERT_EQUAL_HEX32(p.broken_next_hop, out.broken_next_hop);
}

/* ---- BEACON ---- */
void test_beacon_roundtrip(void) {
    bramble_beacon_t p = {
        .header = make_header(PKT_TYPE_BEACON),
        .src_addr = 0xAAAA1111, .pubkey_hash = 0xBBBB2222,
        .uptime_min = 1234, .battery_pct = 85, .tx_queue_depth = 3,
        .neighbor_count = 7, .flags = 0x0F,
        .network_time = 0xCAFEBABE, .time_confidence = 500,
        .auth_hmac = {0x01, 0x02, 0x03, 0x04},
    };
    uint8_t buf[BEACON_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_beacon_serialize(&p, buf, sizeof(buf)));
    bramble_beacon_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_beacon_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX32(p.src_addr, out.src_addr);
    TEST_ASSERT_EQUAL_HEX32(p.pubkey_hash, out.pubkey_hash);
    TEST_ASSERT_EQUAL(p.uptime_min, out.uptime_min);
    TEST_ASSERT_EQUAL(p.battery_pct, out.battery_pct);
    TEST_ASSERT_EQUAL(p.tx_queue_depth, out.tx_queue_depth);
    TEST_ASSERT_EQUAL(p.neighbor_count, out.neighbor_count);
    TEST_ASSERT_EQUAL(p.flags, out.flags);
    TEST_ASSERT_EQUAL_HEX32(p.network_time, out.network_time);
    TEST_ASSERT_EQUAL(p.time_confidence, out.time_confidence);
    TEST_ASSERT_EQUAL_MEMORY(p.auth_hmac, out.auth_hmac, 4);
}

void test_beacon_wire_format(void) {
    bramble_beacon_t p = {
        .header = make_header(PKT_TYPE_BEACON),
        .src_addr = 0x01020304, .pubkey_hash = 0x05060708,
        .uptime_min = 0x0A0B, .battery_pct = 85,
        .network_time = 0xCAFEBABE, .time_confidence = 0x1234,
    };
    uint8_t buf[BEACON_SIZE] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, bramble_beacon_serialize(&p, buf, sizeof(buf)));
    /* uptime_min BE at offset 20 */
    TEST_ASSERT_EQUAL_HEX8(0x0A, buf[20]);
    TEST_ASSERT_EQUAL_HEX8(0x0B, buf[21]);
    /* network_time BE at offset 26 */
    TEST_ASSERT_EQUAL_HEX8(0xCA, buf[26]);
    TEST_ASSERT_EQUAL_HEX8(0xFE, buf[27]);
    TEST_ASSERT_EQUAL_HEX8(0xBA, buf[28]);
    TEST_ASSERT_EQUAL_HEX8(0xBE, buf[29]);
    /* time_confidence BE at offset 30 */
    TEST_ASSERT_EQUAL_HEX8(0x12, buf[30]);
    TEST_ASSERT_EQUAL_HEX8(0x34, buf[31]);
}

/* ---- KEY_EXCHANGE ---- */
void test_key_exchange_roundtrip(void) {
    bramble_key_exchange_t p = {
        .header = make_header(PKT_TYPE_KEY_EXCHANGE),
        .src_addr = 0x44556677, .key_id = 0xAB, .ke_type = 0x01,
    };
    for (int i = 0; i < 32; i++) p.ephemeral_pubkey[i] = (uint8_t)i;
    for (int i = 0; i < 32; i++) p.long_term_pubkey[i] = (uint8_t)(0x80 + i);
    for (int i = 0; i < 16; i++) p.auth_tag[i] = (uint8_t)(0xF0 + i);
    uint8_t buf[KEY_EXCHANGE_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_key_exchange_serialize(&p, buf, sizeof(buf)));
    bramble_key_exchange_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_key_exchange_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX32(p.src_addr, out.src_addr);
    TEST_ASSERT_EQUAL_MEMORY(p.ephemeral_pubkey, out.ephemeral_pubkey, 32);
    TEST_ASSERT_EQUAL_MEMORY(p.long_term_pubkey, out.long_term_pubkey, 32);
    TEST_ASSERT_EQUAL(p.key_id, out.key_id);
    TEST_ASSERT_EQUAL(p.ke_type, out.ke_type);
    TEST_ASSERT_EQUAL_MEMORY(p.auth_tag, out.auth_tag, 16);
}

/* ---- DELIVERY_RECEIPT ---- */
void test_delivery_receipt_roundtrip(void) {
    bramble_delivery_receipt_t p = {
        .header = make_header(PKT_TYPE_DELIVERY_RECEIPT),
        .src_addr = 0x11223344, .orig_packet_id = 0x55667788,
        .hop_count = 3, .total_latency = 200,
        .relay_path = {0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC},
    };
    uint8_t buf[DELIVERY_RECEIPT_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_delivery_receipt_serialize(&p, buf, sizeof(buf)));
    bramble_delivery_receipt_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_delivery_receipt_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX32(p.src_addr, out.src_addr);
    TEST_ASSERT_EQUAL_HEX32(p.orig_packet_id, out.orig_packet_id);
    TEST_ASSERT_EQUAL(p.hop_count, out.hop_count);
    TEST_ASSERT_EQUAL(p.total_latency, out.total_latency);
    for (int i = 0; i < p.hop_count; i++) {
        TEST_ASSERT_EQUAL_HEX32(p.relay_path[i], out.relay_path[i]);
    }
}

void test_delivery_receipt_zero_hops(void) {
    bramble_delivery_receipt_t p = {
        .header = make_header(PKT_TYPE_DELIVERY_RECEIPT),
        .src_addr = 0x11223344, .orig_packet_id = 0x55667788,
        .hop_count = 0, .total_latency = 10,
    };
    /* Max-size backing array, min-size length argument: still proves the
     * zero-hop wire contract, without tripping GCC -Warray-bounds on the
     * (unreachable) relay-path writes it cannot rule out at -O3. */
    uint8_t buf[DELIVERY_RECEIPT_MIN_SIZE + DELIVERY_RECEIPT_MAX_HOPS * 4] = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
                      bramble_delivery_receipt_serialize(&p, buf, DELIVERY_RECEIPT_MIN_SIZE));
    bramble_delivery_receipt_t out;
    TEST_ASSERT_EQUAL(ESP_OK,
                      bramble_delivery_receipt_deserialize(&out, buf, DELIVERY_RECEIPT_MIN_SIZE));
    TEST_ASSERT_EQUAL(0, out.hop_count);
}

void test_location_packet_header_roundtrip(void) {
    bramble_header_t h = make_header(PKT_TYPE_LOCATION);
    h.flags = (uint8_t)(LOCATION_TIER_COARSE << FLAG_TIER_SHIFT);

    uint8_t buf[HEADER_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_serialize(&h, buf, sizeof(buf)));

    bramble_header_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(PKT_TYPE_LOCATION, out.type);
    TEST_ASSERT_EQUAL(LOCATION_TIER_COARSE, (out.flags & FLAG_TIER_MASK) >> FLAG_TIER_SHIFT);
}

void test_location_packet_header_preserves_requested_tier(void) {
    bramble_header_t h = make_header(PKT_TYPE_LOCATION);
    h.flags = (uint8_t)(LOCATION_TIER_PRESENCE << FLAG_TIER_SHIFT);

    uint8_t buf[HEADER_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_serialize(&h, buf, sizeof(buf)));

    bramble_header_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(PKT_TYPE_LOCATION, out.type);
    TEST_ASSERT_EQUAL(LOCATION_TIER_PRESENCE, (out.flags & FLAG_TIER_MASK) >> FLAG_TIER_SHIFT);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_header_roundtrip);
    RUN_TEST(test_header_buffer_too_small);
    RUN_TEST(test_header_big_endian_wire);
    RUN_TEST(test_ack_roundtrip);
    RUN_TEST(test_ack_buffer_too_small);
    RUN_TEST(test_rreq_roundtrip);
    RUN_TEST(test_rrep_roundtrip);
    RUN_TEST(test_rerr_roundtrip);
    RUN_TEST(test_beacon_roundtrip);
    RUN_TEST(test_beacon_wire_format);
    RUN_TEST(test_key_exchange_roundtrip);
    RUN_TEST(test_delivery_receipt_roundtrip);
    RUN_TEST(test_delivery_receipt_zero_hops);
    RUN_TEST(test_location_packet_header_roundtrip);
    RUN_TEST(test_location_packet_header_preserves_requested_tier);
    return UNITY_END();
}
