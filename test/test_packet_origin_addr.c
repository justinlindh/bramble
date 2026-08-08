/*
 * Host coverage for bramble_packet_origin_addr.
 *
 * Traffic events record an RSSI per received packet but carried no source
 * address, so a signal-strength sample could not be attributed to a peer.
 * This extractor supplies that address, and it is easy to get subtly wrong:
 * two encodings share wire offset HEADER_SIZE. Types framed by
 * bramble_*_serialize write src_addr big-endian via put_be32, while the
 * hand-built envelope types (DATA, LOCATION, and the PROBE pair) memcpy it in
 * host order. Reading one with the other's rule yields a byte-swapped address
 * that still looks like a plausible node ID, so these cases are pinned here.
 */

#include "unity.h"

#include <string.h>

#include "packet.h"

void setUp(void) {}
void tearDown(void) {}

#define TEST_ADDR 0x10B76F29u

static void build_be(uint8_t* buf, size_t len, uint32_t addr) {
    memset(buf, 0, len);
    buf[HEADER_SIZE + 0] = (uint8_t)(addr >> 24);
    buf[HEADER_SIZE + 1] = (uint8_t)(addr >> 16);
    buf[HEADER_SIZE + 2] = (uint8_t)(addr >> 8);
    buf[HEADER_SIZE + 3] = (uint8_t)(addr);
}

static void build_host(uint8_t* buf, size_t len, uint32_t addr) {
    memset(buf, 0, len);
    memcpy(buf + HEADER_SIZE, &addr, 4);
}

/* ── Big-endian types (framed by bramble_*_serialize) ─────────────── */

void test_beacon_origin_is_big_endian(void) {
    uint8_t buf[64];
    build_be(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0;
    TEST_ASSERT_TRUE(bramble_packet_origin_addr(PKT_TYPE_BEACON, buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX32(TEST_ADDR, out);
}

void test_ack_origin_is_big_endian(void) {
    uint8_t buf[64];
    build_be(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0;
    TEST_ASSERT_TRUE(bramble_packet_origin_addr(PKT_TYPE_ACK, buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX32(TEST_ADDR, out);
}

void test_delivery_receipt_origin_is_big_endian(void) {
    uint8_t buf[64];
    build_be(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0;
    TEST_ASSERT_TRUE(bramble_packet_origin_addr(PKT_TYPE_DELIVERY_RECEIPT, buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX32(TEST_ADDR, out);
}

void test_identity_attestation_origin_is_big_endian(void) {
    uint8_t buf[64];
    build_be(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0;
    TEST_ASSERT_TRUE(
        bramble_packet_origin_addr(PKT_TYPE_IDENTITY_ATTESTATION, buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX32(TEST_ADDR, out);
}

/* ── Host-order types (hand-built envelopes) ──────────────────────── */

void test_data_origin_is_host_order(void) {
    uint8_t buf[64];
    build_host(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0;
    TEST_ASSERT_TRUE(bramble_packet_origin_addr(PKT_TYPE_DATA, buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX32(TEST_ADDR, out);
}

void test_location_origin_is_host_order(void) {
    uint8_t buf[64];
    build_host(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0;
    TEST_ASSERT_TRUE(bramble_packet_origin_addr(PKT_TYPE_LOCATION, buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX32(TEST_ADDR, out);
}

void test_probe_origin_is_host_order(void) {
    uint8_t buf[64];
    build_host(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0;
    TEST_ASSERT_TRUE(bramble_packet_origin_addr(PKT_TYPE_PROBE, buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX32(TEST_ADDR, out);
}

void test_probe_ack_origin_is_host_order(void) {
    uint8_t buf[64];
    build_host(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0;
    TEST_ASSERT_TRUE(bramble_packet_origin_addr(PKT_TYPE_PROBE_ACK, buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX32(TEST_ADDR, out);
}

/* ── Types that carry no origin at that offset ────────────────────── */

/* RREQ leads with query_id, RREP with query_id, RERR with reporter_addr.
 * Reading offset 12 on any of them yields a plausible-looking but wrong
 * address, which is worse for attribution than reporting nothing. */
void test_rreq_has_no_origin(void) {
    uint8_t buf[64];
    build_be(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0xDEADBEEFu;
    TEST_ASSERT_FALSE(bramble_packet_origin_addr(PKT_TYPE_RREQ, buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, out); /* left untouched */
}

void test_rrep_has_no_origin(void) {
    uint8_t buf[64];
    build_be(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0;
    TEST_ASSERT_FALSE(bramble_packet_origin_addr(PKT_TYPE_RREP, buf, sizeof(buf), &out));
}

void test_rerr_has_no_origin(void) {
    uint8_t buf[64];
    build_be(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0;
    TEST_ASSERT_FALSE(bramble_packet_origin_addr(PKT_TYPE_RERR, buf, sizeof(buf), &out));
}

void test_unknown_type_has_no_origin(void) {
    uint8_t buf[64];
    build_be(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0;
    TEST_ASSERT_FALSE(bramble_packet_origin_addr(0xFE, buf, sizeof(buf), &out));
}

/* ── Bounds and argument safety ───────────────────────────────────── */

/* A runt frame must not be read past its end. */
void test_short_frame_rejected(void) {
    uint8_t buf[HEADER_SIZE + 3];
    memset(buf, 0, sizeof(buf));
    uint32_t out = 0;
    TEST_ASSERT_FALSE(bramble_packet_origin_addr(PKT_TYPE_BEACON, buf, sizeof(buf), &out));
}

void test_exact_minimum_length_accepted(void) {
    uint8_t buf[HEADER_SIZE + 4];
    build_be(buf, sizeof(buf), TEST_ADDR);
    uint32_t out = 0;
    TEST_ASSERT_TRUE(bramble_packet_origin_addr(PKT_TYPE_BEACON, buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX32(TEST_ADDR, out);
}

void test_null_arguments_rejected(void) {
    uint8_t buf[64];
    uint32_t out = 0;
    build_be(buf, sizeof(buf), TEST_ADDR);
    TEST_ASSERT_FALSE(bramble_packet_origin_addr(PKT_TYPE_BEACON, NULL, sizeof(buf), &out));
    TEST_ASSERT_FALSE(bramble_packet_origin_addr(PKT_TYPE_BEACON, buf, sizeof(buf), NULL));
}

/* Zero is the caller's "unknown" sentinel, so a frame genuinely claiming
 * address 0 must still report true rather than being indistinguishable from a
 * type that carries nothing. */
void test_zero_address_still_reports_present(void) {
    uint8_t buf[64];
    build_be(buf, sizeof(buf), 0);
    uint32_t out = 0xFFFFFFFFu;
    TEST_ASSERT_TRUE(bramble_packet_origin_addr(PKT_TYPE_BEACON, buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX32(0u, out);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_beacon_origin_is_big_endian);
    RUN_TEST(test_ack_origin_is_big_endian);
    RUN_TEST(test_delivery_receipt_origin_is_big_endian);
    RUN_TEST(test_identity_attestation_origin_is_big_endian);

    RUN_TEST(test_data_origin_is_host_order);
    RUN_TEST(test_location_origin_is_host_order);
    RUN_TEST(test_probe_origin_is_host_order);
    RUN_TEST(test_probe_ack_origin_is_host_order);

    RUN_TEST(test_rreq_has_no_origin);
    RUN_TEST(test_rrep_has_no_origin);
    RUN_TEST(test_rerr_has_no_origin);
    RUN_TEST(test_unknown_type_has_no_origin);

    RUN_TEST(test_short_frame_rejected);
    RUN_TEST(test_exact_minimum_length_accepted);
    RUN_TEST(test_null_arguments_rejected);
    RUN_TEST(test_zero_address_still_reports_present);

    return UNITY_END();
}
