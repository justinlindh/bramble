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
 *
 * Phase 3 grew the OUTER frame by the relay-gate MAC + seq (auth_hmac(8)
 * + seq(6), 144 -> 158 bytes); the trust-anchor campaign (P1) grew it
 * again by the inline endorsement cert (not_after(8) + endorsement_sig(64),
 * 158 -> 230 bytes). The canonical SIGNED bytes above are UNCHANGED, and
 * the tests below additionally pin that neither the relay-privilege fields
 * (auth_hmac/seq) NOR the cert fields (not_after/endorsement_sig) feed the
 * signed message: the cert is the ANCHOR's signature over ed25519_pub, not
 * part of what the node self-signs. The MAC's coverage of the cert is
 * pinned in test_ident_relay_auth.c.
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
    for (int i = 0; i < 8; i++)
        p->auth_hmac[i] = (uint8_t)(0xC0 + i); /* pattern; real MAC in test_ident_relay_auth.c */
    for (int i = 0; i < 6; i++)
        p->seq[i] = (uint8_t)(0xD0 + i);
    /* Endorsement cert (P1): pattern-filled so round-trip/offset tests are
     * non-vacuous. not_after = 0x0102...08 big-endian. */
    p->not_after = 0x0102030405060708ULL;
    for (int i = 0; i < 64; i++)
        p->endorsement_sig[i] = (uint8_t)(0x20 + i);
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair(p->ed25519_pub, sk));
    if (sign) {
        uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
        TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(p, msg, sizeof(msg)));
        TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(sk, msg, sizeof(msg), p->sig));
    }
}

/* ── Wire layout ────────────────────────────────────────────────────── */

static void test_wire_size_is_230(void) {
    TEST_ASSERT_EQUAL(230, IDENTITY_ATTESTATION_SIZE);
    TEST_ASSERT_EQUAL(HEADER_SIZE + 4 + 32 + 32 + 64 + 8 + 64 + 8 + 6, IDENTITY_ATTESTATION_SIZE);
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
    /* x25519_pub at 16, ed25519_pub at 48, sig at 80, not_after at 144,
     * endorsement_sig at 152 (P1 cert), auth_hmac at 216, seq at 224. */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.x25519_pub, buf + 16, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.ed25519_pub, buf + 48, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.sig, buf + 80, 64);
    /* not_after big-endian at 144 */
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[144]);
    TEST_ASSERT_EQUAL_HEX8(0x02, buf[145]);
    TEST_ASSERT_EQUAL_HEX8(0x03, buf[146]);
    TEST_ASSERT_EQUAL_HEX8(0x04, buf[147]);
    TEST_ASSERT_EQUAL_HEX8(0x05, buf[148]);
    TEST_ASSERT_EQUAL_HEX8(0x06, buf[149]);
    TEST_ASSERT_EQUAL_HEX8(0x07, buf[150]);
    TEST_ASSERT_EQUAL_HEX8(0x08, buf[151]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.endorsement_sig, buf + 152, 64);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.auth_hmac, buf + 216, 8);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.seq, buf + 224, 6);
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
    TEST_ASSERT_EQUAL_HEX64(p.not_after, q.not_after);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.endorsement_sig, q.endorsement_sig, 64);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.auth_hmac, q.auth_hmac, 8);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p.seq, q.seq, 6);
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
    /* Flag day: the old 158-byte frame and the off-by-one neighbours 229/231
     * are all rejected. 230 is the only accepted length. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, bramble_identity_attestation_deserialize(&q, buf, 158));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, bramble_identity_attestation_deserialize(&q, buf, 229));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, bramble_identity_attestation_deserialize(&q, buf, 231));
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
     * field and re-derive; bytes are identical. Phase 3: the outer
     * relay-gate fields (auth_hmac/seq) must not influence it either; the
     * canonical SIGNED bytes are the P2 contract, unchanged. */
    bramble_identity_attestation_t p2 = p;
    p2.header.hop_limit = 1;
    p2.header.packet_id = 0xDEADBEEFu;
    p2.header.flags = 0xFF;
    p2.header.dest_addr = 0x12345678u;
    memset(p2.auth_hmac, 0x5A, sizeof(p2.auth_hmac));
    memset(p2.seq, 0xA5, sizeof(p2.seq));
    /* P1: the endorsement cert (anchor's signature) is NOT part of what the
     * node self-signs, so mutating it must not perturb the signed message. */
    p2.not_after = 0xFFFFFFFFFFFFFFFFULL;
    memset(p2.endorsement_sig, 0x77, sizeof(p2.endorsement_sig));
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

/* Origination contract: an attestation built from a node's own
 * bramble_identity_t, using exactly the field mapping
 * send_identity_attestation (main/mesh_beacon.c) uses, verifies against the
 * frame's embedded key after a wire round trip. mesh_task.c itself is
 * never host-compiled (see test/CMakeLists.txt), so this is the host-side
 * pin of the identity-to-frame mapping. */
static void test_origination_from_node_identity_verifies(void) {
    bramble_identity_t id;
    TEST_ASSERT_EQUAL(0, crypto_generate_identity(&id));

    bramble_identity_attestation_t att;
    memset(&att, 0, sizeof(att));
    att.header.version = BRAMBLE_VERSION;
    att.header.type = PKT_TYPE_IDENTITY_ATTESTATION;
    att.header.flags = 0;
    att.header.hop_limit = 8;
    att.header.dest_addr = 0xFFFFFFFFu;
    att.header.packet_id = 42;
    att.src_addr = id.address;
    memcpy(att.x25519_pub, id.public_key, sizeof(att.x25519_pub));
    memcpy(att.ed25519_pub, id.ed25519_public_key, sizeof(att.ed25519_pub));

    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(&att, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(id.ed25519_private_key, msg, sizeof(msg), att.sig));

    uint8_t buf[IDENTITY_ATTESTATION_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_serialize(&att, buf, sizeof(buf)));
    bramble_identity_attestation_t rx;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_deserialize(&rx, buf, sizeof(buf)));

    TEST_ASSERT_EQUAL_HEX32(id.address, rx.src_addr);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(id.public_key, rx.x25519_pub, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(id.ed25519_public_key, rx.ed25519_pub, 32);
    uint8_t msg2[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(&rx, msg2, sizeof(msg2)));
    TEST_ASSERT_TRUE(crypto_ed25519_verify(rx.ed25519_pub, msg2, sizeof(msg2), rx.sig));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_wire_size_is_230);
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
    RUN_TEST(test_origination_from_node_identity_verifies);
    return UNITY_END();
}
