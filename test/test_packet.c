#include "unity.h"
#include "esp_stubs.h"
#include "packet.h"
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
        .auth_hmac = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04},
        .hop_count = 2, .relay_path = {0x10101010, 0x20202020},
    };
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&p, buf, sizeof(buf)));
    bramble_ack_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX32(p.src_addr, out.src_addr);
    TEST_ASSERT_EQUAL_HEX32(p.ack_packet_id, out.ack_packet_id);
    TEST_ASSERT_EQUAL(p.ack_flags, out.ack_flags);
    TEST_ASSERT_EQUAL(p.rssi_at_dest, out.rssi_at_dest);
    /* auth_hmac lives at a fixed offset BEFORE relay_path (NEW-SEC-8):
     * round-tripping both together, with a non-zero hop_count, proves the
     * two fields don't collide or get corrupted at the new layout. */
    TEST_ASSERT_EQUAL_MEMORY(p.auth_hmac, out.auth_hmac, sizeof(p.auth_hmac));
    TEST_ASSERT_EQUAL(p.hop_count, out.hop_count);
    TEST_ASSERT_EQUAL_HEX32(p.relay_path[0], out.relay_path[0]);
    TEST_ASSERT_EQUAL_HEX32(p.relay_path[1], out.relay_path[1]);
}

/* auth_hmac must sit at the SAME wire offset regardless of hop_count
 * (NEW-SEC-8): a verifier reads it before it can trust hop_count at all.
 * Serializes the same auth_hmac under two different hop_counts and
 * confirms it lands at the identical byte offset both times. */
void test_ack_auth_hmac_offset_independent_of_hop_count(void) {
    bramble_ack_t p_zero_hops = {
        .header = make_header(PKT_TYPE_ACK),
        .src_addr = 0x1, .ack_packet_id = 0x2,
        .auth_hmac = {1, 2, 3, 4, 5, 6, 7, 8},
        .hop_count = 0,
    };
    bramble_ack_t p_many_hops = p_zero_hops;
    p_many_hops.hop_count = 4;
    for (int i = 0; i < 4; i++) p_many_hops.relay_path[i] = 0x30000000 + i;

    uint8_t buf_zero[ACK_MAX_SIZE];
    uint8_t buf_many[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&p_zero_hops, buf_zero, sizeof(buf_zero)));
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&p_many_hops, buf_many, sizeof(buf_many)));

    /* Fixed offset: HEADER_SIZE + src(4) + ack_pkt_id(4) + flags(1) +
     * rssi(1) + hop_count(1) = HEADER_SIZE + 11. */
    size_t hmac_offset = HEADER_SIZE + 11;
    TEST_ASSERT_EQUAL_MEMORY(buf_zero + hmac_offset, buf_many + hmac_offset, 8);
    TEST_ASSERT_EQUAL_MEMORY(p_zero_hops.auth_hmac, buf_many + hmac_offset, 8);
}

/* Fix 5 (red-team panel): a tampered/truncated hop_count claiming more
 * relay_path entries than the buffer actually supplies must not leave
 * relay_path[] beyond what was truly read holding uninitialized/garbage
 * caller memory. handle_ack (main/mesh_task.c) reads relay_path[0..
 * hop_count) straight into the onAck UI notification, so stale bytes
 * there is a real (bounded, own-UI) leak. This proves hop_count is
 * clamped DOWN to the number of entries the buffer truly carries, and
 * that the untouched tail of relay_path reads zero, not whatever was on
 * the stack before deserialize was called. */
void test_ack_deserialize_clamps_hop_count_to_available_bytes(void) {
    bramble_ack_t p = {
        .header = make_header(PKT_TYPE_ACK),
        .src_addr = 0x1, .ack_packet_id = 0x2,
        .hop_count = ACK_MAX_HOPS,
    };
    for (int i = 0; i < ACK_MAX_HOPS; i++) p.relay_path[i] = 0x40000000 + i;
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&p, buf, sizeof(buf)));

    /* Truncate to only 2 hops' worth of wire bytes, as if the packet on
     * the wire were shorter than hop_count=8 claims (attacker-tampered or
     * genuinely truncated in transit). */
    size_t truncated_len = ACK_BASE_SIZE + 2 * 4;

    bramble_ack_t out;
    memset(&out, 0xAA, sizeof(out)); /* poison: simulate stack garbage */
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_deserialize(&out, buf, truncated_len));

    TEST_ASSERT_EQUAL(2, out.hop_count); /* clamped to what the buffer truly carries */
    TEST_ASSERT_EQUAL_HEX32(0x40000000, out.relay_path[0]);
    TEST_ASSERT_EQUAL_HEX32(0x40000001, out.relay_path[1]);
    for (int i = 2; i < ACK_MAX_HOPS; i++) {
        TEST_ASSERT_EQUAL_HEX32(0, out.relay_path[i]); /* zeroed, not 0xAAAAAAAA poison */
    }
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
        .auth_hmac = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04},
    };
    uint8_t buf[RERR_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rerr_serialize(&p, buf, sizeof(buf)));
    bramble_rerr_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rerr_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX32(p.reporter_addr, out.reporter_addr);
    TEST_ASSERT_EQUAL_HEX32(p.broken_dest, out.broken_dest);
    TEST_ASSERT_EQUAL_HEX32(p.broken_next_hop, out.broken_next_hop);
    TEST_ASSERT_EQUAL_MEMORY(p.auth_hmac, out.auth_hmac, sizeof(p.auth_hmac));
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
        .auth_hmac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x10, 0x20},
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
    /* auth_hmac lives at a fixed offset BEFORE relay_path (NEW-SEC-8):
     * round-tripping both together, with a non-zero hop_count, proves the
     * two fields don't collide or get corrupted at the new layout. */
    TEST_ASSERT_EQUAL_MEMORY(p.auth_hmac, out.auth_hmac, sizeof(p.auth_hmac));
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_header_roundtrip);
    RUN_TEST(test_header_buffer_too_small);
    RUN_TEST(test_header_big_endian_wire);
    RUN_TEST(test_ack_roundtrip);
    RUN_TEST(test_ack_auth_hmac_offset_independent_of_hop_count);
    RUN_TEST(test_ack_deserialize_clamps_hop_count_to_available_bytes);
    RUN_TEST(test_ack_buffer_too_small);
    RUN_TEST(test_rreq_roundtrip);
    RUN_TEST(test_rrep_roundtrip);
    RUN_TEST(test_rerr_roundtrip);
    RUN_TEST(test_beacon_roundtrip);
    RUN_TEST(test_beacon_wire_format);
    RUN_TEST(test_key_exchange_roundtrip);
    RUN_TEST(test_delivery_receipt_roundtrip);
    RUN_TEST(test_delivery_receipt_zero_hops);
    return UNITY_END();
}
