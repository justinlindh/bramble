#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "network_key.h"
#include "routing_auth.h"
#include "packet.h"

#include "../components/crypto/crypto_host.c"
#include "../components/network_key/network_key.c"
#include "../components/packet/packet.c"
#include "../components/routing_auth/routing_auth.c"

/*
 * Task 3.5 / NEW-SEC-8 (STAGED, not closed: see network_key.h). ack_sign/
 * ack_verify and receipt_sign/receipt_verify authenticate
 * src_addr||ack_packet_id (or src_addr||orig_packet_id), deliberately
 * excluding relay_path/hop_count/header.hop_limit: mesh_task.c's
 * forward_ack and forward_delivery_receipt both grow relay_path,
 * increment hop_count, and decrement hop_limit on every relay hop.
 * Confirmed by reading both forwarding functions directly before writing
 * any code here.
 */

void setUp(void) { network_key_clear(); }
void tearDown(void) {}

/* ws 1.3b: the seq is set by the caller (send_ack/the receipt builder
 * draw it via control_seq_next and write it in before ack_sign/
 * receipt_sign), mirroring the RREP/RERR test files' set_seq helpers. */
static void set_ack_seq(bramble_ack_t *a, uint64_t seq) {
    a->seq[0] = (uint8_t)(seq >> 40);
    a->seq[1] = (uint8_t)(seq >> 32);
    a->seq[2] = (uint8_t)(seq >> 24);
    a->seq[3] = (uint8_t)(seq >> 16);
    a->seq[4] = (uint8_t)(seq >> 8);
    a->seq[5] = (uint8_t)seq;
}

static bramble_ack_t make_ack(uint32_t src_addr, uint32_t ack_packet_id) {
    bramble_ack_t a = {0};
    a.header.version = BRAMBLE_VERSION;
    a.header.type = PKT_TYPE_ACK;
    a.header.hop_limit = 8;
    a.header.dest_addr = 0x99999999;
    a.header.packet_id = 0x12345678;
    a.src_addr = src_addr;
    a.ack_packet_id = ack_packet_id;
    a.ack_flags = 0;
    a.rssi_at_dest = -50;
    a.hop_count = 1;
    a.relay_path[0] = src_addr;
    return a;
}

static void set_receipt_seq(bramble_delivery_receipt_t *r, uint64_t seq) {
    r->seq[0] = (uint8_t)(seq >> 40);
    r->seq[1] = (uint8_t)(seq >> 32);
    r->seq[2] = (uint8_t)(seq >> 24);
    r->seq[3] = (uint8_t)(seq >> 16);
    r->seq[4] = (uint8_t)(seq >> 8);
    r->seq[5] = (uint8_t)seq;
}

static bramble_delivery_receipt_t make_receipt(uint32_t src_addr, uint32_t orig_packet_id) {
    bramble_delivery_receipt_t r = {0};
    r.header.version = BRAMBLE_VERSION;
    r.header.type = PKT_TYPE_DELIVERY_RECEIPT;
    r.header.hop_limit = 8;
    r.header.dest_addr = 0x99999999;
    r.header.packet_id = 0x87654321;
    r.src_addr = src_addr;
    r.orig_packet_id = orig_packet_id;
    r.hop_count = 1;
    r.total_latency = 0;
    r.relay_path[0] = src_addr;
    return r;
}

/* ── ACK ────────────────────────────────────────────────────────── */

void test_ack_sign_verify_round_trip(void) {
    bramble_ack_t a = make_ack(0x11111111, 0x2222);
    ack_sign(&a);
    TEST_ASSERT_TRUE(ack_verify(&a));
}

void test_ack_verify_rejects_tampered_ack_packet_id(void) {
    bramble_ack_t a = make_ack(0x11111111, 0x2222);
    ack_sign(&a);
    a.ack_packet_id ^= 0xFFFFFFFF; /* tamper an origin-stable field after signing */
    TEST_ASSERT_FALSE(ack_verify(&a));
}

/*
 * The key regression case. forward_ack grows relay_path, increments
 * hop_count, and decrements header.hop_limit. Simulate that exactly and
 * confirm the ORIGINAL auth_hmac still verifies.
 */
void test_ack_verify_survives_forwarding(void) {
    bramble_ack_t a = make_ack(0x11111111, 0x2222);
    set_ack_seq(&a, 0x0102030405);
    ack_sign(&a);

    bramble_ack_t forwarded = a;
    forwarded.relay_path[forwarded.hop_count++] = 0x33333333;
    forwarded.header.hop_limit--;
    /* Sanity: the fields actually changed, or this test would pass
     * vacuously without exercising anything. */
    TEST_ASSERT_NOT_EQUAL(a.hop_count, forwarded.hop_count);
    TEST_ASSERT_NOT_EQUAL(a.header.hop_limit, forwarded.header.hop_limit);
    /* seq is origin-stable: forward_ack carries it through unchanged, same
     * as auth_hmac. */
    TEST_ASSERT_EQUAL_MEMORY(a.seq, forwarded.seq, sizeof(a.seq));

    TEST_ASSERT_TRUE(ack_verify(&forwarded));
}

/* ws 1.3b: the 48-bit seq is part of the auth buffer (ack_build_auth_buf),
 * so tampering it after signing must break verification exactly like
 * tampering ack_packet_id does above. */
void test_ack_seq_covered(void) {
    bramble_ack_t a = make_ack(0x11111111, 0x2222);
    set_ack_seq(&a, 0x0102030405);
    ack_sign(&a);
    TEST_ASSERT_TRUE(ack_verify(&a)); /* sanity: correctly signed with the seq included */

    a.seq[5] ^= 0xFF; /* tamper the seq after signing */
    TEST_ASSERT_FALSE(ack_verify(&a));
}

void test_ack_verify_rejects_wrong_key_forgery(void) {
    bramble_ack_t a = make_ack(0x11111111, 0x2222);

    uint8_t attacker_key[32];
    crypto_random(attacker_key, 32);
    network_key_set_provisioned(attacker_key);
    ack_sign(&a);

    network_key_clear();
    TEST_ASSERT_FALSE(ack_verify(&a));
}

/* ── Delivery receipt ───────────────────────────────────────────── */

void test_receipt_sign_verify_round_trip(void) {
    bramble_delivery_receipt_t r = make_receipt(0x44444444, 0x5555);
    receipt_sign(&r);
    TEST_ASSERT_TRUE(receipt_verify(&r));
}

void test_receipt_verify_rejects_tampered_orig_packet_id(void) {
    bramble_delivery_receipt_t r = make_receipt(0x44444444, 0x5555);
    receipt_sign(&r);
    r.orig_packet_id ^= 0xFFFFFFFF;
    TEST_ASSERT_FALSE(receipt_verify(&r));
}

/*
 * The key regression case. forward_delivery_receipt grows relay_path,
 * increments hop_count, and decrements header.hop_limit.
 */
void test_receipt_verify_survives_forwarding(void) {
    bramble_delivery_receipt_t r = make_receipt(0x44444444, 0x5555);
    set_receipt_seq(&r, 0x0102030405);
    receipt_sign(&r);

    bramble_delivery_receipt_t forwarded = r;
    forwarded.relay_path[forwarded.hop_count++] = 0x66666666;
    forwarded.header.hop_limit--;
    TEST_ASSERT_NOT_EQUAL(r.hop_count, forwarded.hop_count);
    TEST_ASSERT_NOT_EQUAL(r.header.hop_limit, forwarded.header.hop_limit);
    /* seq is origin-stable: forward_delivery_receipt carries it through
     * unchanged, same as auth_hmac. */
    TEST_ASSERT_EQUAL_MEMORY(r.seq, forwarded.seq, sizeof(r.seq));

    TEST_ASSERT_TRUE(receipt_verify(&forwarded));
}

/* ws 1.3b: the 48-bit seq is part of the auth buffer
 * (receipt_build_auth_buf), so tampering it after signing must break
 * verification exactly like tampering orig_packet_id does above. */
void test_receipt_seq_covered(void) {
    bramble_delivery_receipt_t r = make_receipt(0x44444444, 0x5555);
    set_receipt_seq(&r, 0x0102030405);
    receipt_sign(&r);
    TEST_ASSERT_TRUE(receipt_verify(&r)); /* sanity: correctly signed with the seq included */

    r.seq[5] ^= 0xFF; /* tamper the seq after signing */
    TEST_ASSERT_FALSE(receipt_verify(&r));
}

void test_receipt_verify_rejects_wrong_key_forgery(void) {
    bramble_delivery_receipt_t r = make_receipt(0x44444444, 0x5555);

    uint8_t attacker_key[32];
    crypto_random(attacker_key, 32);
    network_key_set_provisioned(attacker_key);
    receipt_sign(&r);

    network_key_clear();
    TEST_ASSERT_FALSE(receipt_verify(&r));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ack_sign_verify_round_trip);
    RUN_TEST(test_ack_verify_rejects_tampered_ack_packet_id);
    RUN_TEST(test_ack_verify_survives_forwarding);
    RUN_TEST(test_ack_seq_covered);
    RUN_TEST(test_ack_verify_rejects_wrong_key_forgery);
    RUN_TEST(test_receipt_sign_verify_round_trip);
    RUN_TEST(test_receipt_verify_rejects_tampered_orig_packet_id);
    RUN_TEST(test_receipt_verify_survives_forwarding);
    RUN_TEST(test_receipt_seq_covered);
    RUN_TEST(test_receipt_verify_rejects_wrong_key_forgery);
    return UNITY_END();
}
