/*
 * Identity attestation frame (PKT_TYPE_IDENTITY_ATTESTATION, Phase 2).
 *
 * Pins the wire layout at exact offsets, the exact-length deserializer
 * check, and the security core: the Ed25519 signature covers EXACTLY the
 * domain-separated canonical message
 *
 *   "bramble-ident-v1" || src_addr(4, big-endian) || x25519_pub(32)
 *                      || ed25519_pub(32)
 *
 * and NOT the header (hop_limit/packet_id are relay-mutable/per-send).
 * Phase 3's verifier pins these same bytes, so the tamper tests here are
 * the contract: flipping any covered field must fail verification.
 */
#include "unity.h"
#include "packet.h"
#include "crypto.h"

void setUp(void) {}
void tearDown(void) {}

/* Deterministic fixture: a fully populated attestation with a freshly
 * generated Ed25519 keypair and pattern-filled X25519 pub. sign=true also
 * signs the canonical message with the generated secret key. */
static void make_attestation(bramble_identity_attestation_t* p, uint8_t sk[64], bool sign) {
    memset(p, 0, sizeof(*p));
    p->header.version = BRAMBLE_VERSION;
    p->header.type = PKT_TYPE_IDENTITY_ATTESTATION;
    p->header.flags = 0;
    p->header.hop_limit = 5;
    p->header.dest_addr = 0xFFFFFFFFu;
    p->header.packet_id = 0xA1B2C3D4u;
    p->src_addr = 0x11223344u;
    for (int i = 0; i < 32; i++)
        p->x25519_pub[i] = (uint8_t)(0x40 + i);
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair(p->ed25519_pub, sk));
    if (sign) {
        uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
        TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(p, msg, sizeof(msg)));
        TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(sk, msg, sizeof(msg), p->sig));
    }
}

/* ── Wire layout ────────────────────────────────────────────────────── */

static void test_wire_size_is_144(void) {
    TEST_ASSERT_EQUAL(144, IDENTITY_ATTESTATION_SIZE);
    TEST_ASSERT_EQUAL(HEADER_SIZE + 4 + 32 + 32 + 64, IDENTITY_ATTESTATION_SIZE);
}

static void test_serialize_exact_offsets(void) {
    bramble_identity_attestation_t p;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_attestation(&p, sk, true);
    for (int i = 0; i < 64; i++)
        p.sig[i] = (uint8_t)(0x80 + i); /* pattern; offsets only here */

    uint8_t buf[IDENTITY_ATTESTATION_SIZE];
    memset(buf, 0xEE, sizeof(buf));
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_serialize(&p, buf, sizeof(buf)));

    /* Header at 0..11 */
    TEST_ASSERT_EQUAL_HEX8(BRAMBLE_VERSION, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(PKT_TYPE_IDENTITY_ATTESTATION, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x15, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(5, buf[3]); /* hop_limit */
    /* src_addr big-endian at 12 */
    TEST_ASSERT_EQUAL_HEX8(0x11, buf[12]);
    TEST_ASSERT_EQUAL_HEX8(0x22, buf[13]);
    TEST_ASSERT_EQUAL_HEX8(0x33, buf[14]);
    TEST_ASSERT_EQUAL_HEX8(0x44, buf[15]);
    /* x25519_pub at 16, ed25519_pub at 48, sig at 80 */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.x25519_pub, buf + 16, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.ed25519_pub, buf + 48, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.sig, buf + 80, 64);
}

static void test_round_trip(void) {
    bramble_identity_attestation_t p, q;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_attestation(&p, sk, true);

    uint8_t buf[IDENTITY_ATTESTATION_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_serialize(&p, buf, sizeof(buf)));
    memset(&q, 0xAB, sizeof(q));
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_deserialize(&q, buf, sizeof(buf)));

    TEST_ASSERT_EQUAL_HEX32(p.header.dest_addr, q.header.dest_addr);
    TEST_ASSERT_EQUAL_HEX32(p.header.packet_id, q.header.packet_id);
    TEST_ASSERT_EQUAL_HEX8(p.header.type, q.header.type);
    TEST_ASSERT_EQUAL_HEX8(p.header.hop_limit, q.header.hop_limit);
    TEST_ASSERT_EQUAL_HEX32(p.src_addr, q.src_addr);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.x25519_pub, q.x25519_pub, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.ed25519_pub, q.ed25519_pub, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.sig, q.sig, 64);
}

static void test_serialize_short_buffer_rejected(void) {
    bramble_identity_attestation_t p;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_attestation(&p, sk, false);
    uint8_t buf[IDENTITY_ATTESTATION_SIZE - 1];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      bramble_identity_attestation_serialize(&p, buf, sizeof(buf)));
}

static void test_deserialize_length_must_be_exact(void) {
    bramble_identity_attestation_t p, q;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_attestation(&p, sk, true);
    uint8_t buf[IDENTITY_ATTESTATION_SIZE + 1];
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL(ESP_OK,
                      bramble_identity_attestation_serialize(&p, buf, IDENTITY_ATTESTATION_SIZE));
    /* Truncated and padded frames are both rejected: length is exact. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, bramble_identity_attestation_deserialize(
                                                &q, buf, IDENTITY_ATTESTATION_SIZE - 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, bramble_identity_attestation_deserialize(
                                                &q, buf, IDENTITY_ATTESTATION_SIZE + 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, bramble_identity_attestation_deserialize(&q, buf, 0));
    TEST_ASSERT_EQUAL(ESP_OK,
                      bramble_identity_attestation_deserialize(&q, buf, IDENTITY_ATTESTATION_SIZE));
}

/* ── Canonical signed message ───────────────────────────────────────── */

static void test_signed_msg_exact_bytes(void) {
    bramble_identity_attestation_t p;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_attestation(&p, sk, false);

    TEST_ASSERT_EQUAL(84, IDENTITY_ATTESTATION_MSG_SIZE);
    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(&p, msg, sizeof(msg)));

    /* Domain-separation context, no NUL terminator */
    TEST_ASSERT_EQUAL_MEMORY("bramble-ident-v1", msg, 16);
    /* src_addr big-endian right after the context */
    TEST_ASSERT_EQUAL_HEX8(0x11, msg[16]);
    TEST_ASSERT_EQUAL_HEX8(0x22, msg[17]);
    TEST_ASSERT_EQUAL_HEX8(0x33, msg[18]);
    TEST_ASSERT_EQUAL_HEX8(0x44, msg[19]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.x25519_pub, msg + 20, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.ed25519_pub, msg + 52, 32);

    /* Header bytes must NOT influence the message: mutate every header
     * field and re-derive; bytes are identical. */
    bramble_identity_attestation_t p2 = p;
    p2.header.hop_limit = 1;
    p2.header.packet_id = 0xDEADBEEFu;
    p2.header.flags = 0xFF;
    p2.header.dest_addr = 0x12345678u;
    uint8_t msg2[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(&p2, msg2, sizeof(msg2)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(msg, msg2, IDENTITY_ATTESTATION_MSG_SIZE);

    uint8_t small[IDENTITY_ATTESTATION_MSG_SIZE - 1];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      bramble_identity_attestation_signed_msg(&p, small, sizeof(small)));
}

/* ── Sign / verify / tamper ─────────────────────────────────────────── */

static void test_signature_verifies_after_wire_round_trip(void) {
    bramble_identity_attestation_t p, rx;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_attestation(&p, sk, true);

    uint8_t buf[IDENTITY_ATTESTATION_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_serialize(&p, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_deserialize(&rx, buf, sizeof(buf)));

    /* Receiver rebuilds the canonical message from the frame's own fields
     * and verifies with the frame's embedded ed25519_pub: exactly what
     * Phase 3 will do. */
    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(&rx, msg, sizeof(msg)));
    TEST_ASSERT_TRUE(crypto_ed25519_verify(rx.ed25519_pub, msg, sizeof(msg), rx.sig));
}

/* Tamper one field of a signed frame, rebuild the canonical message the
 * way a verifier would, and require verification to FAIL. Non-vacuous:
 * the untampered control right before each mutation verifies. */
static void tamper_and_expect_fail(void (*mutate)(bramble_identity_attestation_t*)) {
    bramble_identity_attestation_t p;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_attestation(&p, sk, true);

    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(&p, msg, sizeof(msg)));
    TEST_ASSERT_TRUE(crypto_ed25519_verify(p.ed25519_pub, msg, sizeof(msg), p.sig));

    mutate(&p);
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(&p, msg, sizeof(msg)));
    TEST_ASSERT_FALSE(crypto_ed25519_verify(p.ed25519_pub, msg, sizeof(msg), p.sig));
}

static void mutate_src_addr(bramble_identity_attestation_t* p) { p->src_addr ^= 1u; }
static void mutate_x25519(bramble_identity_attestation_t* p) { p->x25519_pub[7] ^= 0x01; }
static void mutate_ed25519(bramble_identity_attestation_t* p) { p->ed25519_pub[0] ^= 0x01; }

static void test_tampered_src_addr_fails_verify(void) { tamper_and_expect_fail(mutate_src_addr); }
static void test_tampered_x25519_pub_fails_verify(void) { tamper_and_expect_fail(mutate_x25519); }
static void test_tampered_ed25519_pub_fails_verify(void) { tamper_and_expect_fail(mutate_ed25519); }

static void test_tampered_sig_fails_verify(void) {
    bramble_identity_attestation_t p;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_attestation(&p, sk, true);
    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(&p, msg, sizeof(msg)));
    p.sig[63] ^= 0x01;
    TEST_ASSERT_FALSE(crypto_ed25519_verify(p.ed25519_pub, msg, sizeof(msg), p.sig));
}

/* A different node's key cannot vouch for this frame: verify with a
 * fresh, unrelated pubkey fails even though the sig is genuine. */
static void test_wrong_key_fails_verify(void) {
    bramble_identity_attestation_t p;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_attestation(&p, sk, true);
    uint8_t other_pk[BRAMBLE_ED25519_PUBKEY_SIZE];
    uint8_t other_sk[BRAMBLE_ED25519_SECKEY_SIZE];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair(other_pk, other_sk));
    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(&p, msg, sizeof(msg)));
    TEST_ASSERT_FALSE(crypto_ed25519_verify(other_pk, msg, sizeof(msg), p.sig));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_wire_size_is_144);
    RUN_TEST(test_serialize_exact_offsets);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_serialize_short_buffer_rejected);
    RUN_TEST(test_deserialize_length_must_be_exact);
    RUN_TEST(test_signed_msg_exact_bytes);
    RUN_TEST(test_signature_verifies_after_wire_round_trip);
    RUN_TEST(test_tampered_src_addr_fails_verify);
    RUN_TEST(test_tampered_x25519_pub_fails_verify);
    RUN_TEST(test_tampered_ed25519_pub_fails_verify);
    RUN_TEST(test_tampered_sig_fails_verify);
    RUN_TEST(test_wrong_key_fails_verify);
    return UNITY_END();
}
