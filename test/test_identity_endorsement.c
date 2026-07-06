/*
 * Trust-anchor endorsement primitive + anchor-pubkey provisioning (P0).
 *
 * The endorsement KAT pins the canonical 58-byte signed-message layout
 * (context / order / big-endian not_after) to exact signature bytes from a
 * FIXED anchor seed and a FIXED node public key. Because the message layout
 * is baked into the pinned signature, host and device can never silently
 * diverge on it: any change to the context string, field order, or endianness
 * changes the bytes the anchor signs and breaks this vector. Mirrors the
 * RFC 8032 KAT style of test_ed25519.c.
 *
 * The anchor-provisioning tests run against the host in-memory blob store of
 * identity.c (the same save/load path shared with the device NVS backend).
 */
#include "unity.h"
#include "crypto.h"
#include "identity.h"

#include "../components/crypto/crypto_host.c"
#include "../components/identity/identity.c"

/* Fixed KAT inputs. The anchor SEED expands (RFC 8032) to the anchor keypair;
 * the node public key is arbitrary 32-byte data inside the signed message. */
static const uint8_t KAT_ANCHOR_SEED[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint8_t KAT_NODE_PUB[32] = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f};

/* The exact endorsement signature over KAT_NODE_PUB with not_after = PERMANENT
 * (UINT64_MAX), signed by the KAT anchor key. Pins the canonical message. */
static const uint8_t KAT_SIG_PERMANENT[64] = {
    0x01, 0x6e, 0x65, 0xea, 0xe2, 0x69, 0xec, 0x3b, 0x14, 0x65, 0x25, 0x2b, 0x33, 0xd5, 0x26, 0xc1,
    0xd9, 0x15, 0x7d, 0x39, 0xdf, 0xff, 0x5f, 0x90, 0x09, 0xc7, 0x1b, 0xb6, 0x11, 0x8a, 0x85, 0xb3,
    0x7a, 0x36, 0xaf, 0xd2, 0x8b, 0xc2, 0xf3, 0x68, 0x69, 0xf2, 0xbb, 0xa5, 0x4b, 0x60, 0x1c, 0x79,
    0xcc, 0x81, 0x21, 0x3d, 0xcc, 0x2c, 0x41, 0xb7, 0x6e, 0xc3, 0x2a, 0xb7, 0x47, 0x40, 0xb9, 0x03};

void setUp(void) {
    identity_host_store_reset();
    identity_anchor_clear();
}
void tearDown(void) {}

/* Sign an endorsement with the KAT anchor key (test-side signing only). */
static void kat_anchor_keypair(uint8_t anchor_pub[32], uint8_t anchor_priv[64]) {
    TEST_ASSERT_EQUAL(0,
                      crypto_ed25519_keypair_from_seed(KAT_ANCHOR_SEED, anchor_pub, anchor_priv));
}

/* --- Message builder: exact 58-byte layout ------------------------------- */

void test_endorsement_msg_exact_layout(void) {
    uint8_t buf[IDENTITY_ENDORSEMENT_MSG_SIZE];
    /* Distinct, non-sentinel not_after exercises the big-endian encoding. */
    uint64_t not_after = 0x0102030405060708ULL;
    TEST_ASSERT_EQUAL(IDENTITY_ENDORSEMENT_MSG_SIZE,
                      identity_endorsement_msg(KAT_NODE_PUB, not_after, buf, sizeof(buf)));

    /* context(18) */
    TEST_ASSERT_EQUAL_MEMORY("bramble-endorse-v1", buf, 18);
    /* node pubkey(32) */
    TEST_ASSERT_EQUAL_MEMORY(KAT_NODE_PUB, buf + 18, 32);
    /* not_after(8) big-endian */
    static const uint8_t want_be[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    TEST_ASSERT_EQUAL_MEMORY(want_be, buf + 50, 8);
}

void test_endorsement_msg_permanent_sentinel_layout(void) {
    uint8_t buf[IDENTITY_ENDORSEMENT_MSG_SIZE];
    TEST_ASSERT_EQUAL(IDENTITY_ENDORSEMENT_MSG_SIZE,
                      identity_endorsement_msg(KAT_NODE_PUB,
                                               IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT, buf,
                                               sizeof(buf)));
    static const uint8_t all_ff[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    TEST_ASSERT_EQUAL_MEMORY(all_ff, buf + 50, 8);
}

void test_endorsement_msg_rejects_short_buffer(void) {
    uint8_t buf[IDENTITY_ENDORSEMENT_MSG_SIZE - 1];
    TEST_ASSERT_EQUAL(0, identity_endorsement_msg(KAT_NODE_PUB,
                                                  IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT, buf,
                                                  sizeof(buf)));
}

/* --- KAT: fixed seed + node pub + permanent -> pinned signature ---------- */

void test_endorsement_kat_permanent_verifies(void) {
    uint8_t anchor_pub[32], anchor_priv[64];
    kat_anchor_keypair(anchor_pub, anchor_priv);

    /* Sign the canonical message with the anchor key and confirm the exact
     * pinned bytes reproduce (locks the layout on the signing side). */
    uint8_t msg[IDENTITY_ENDORSEMENT_MSG_SIZE];
    TEST_ASSERT_EQUAL(IDENTITY_ENDORSEMENT_MSG_SIZE,
                      identity_endorsement_msg(KAT_NODE_PUB,
                                               IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT, msg,
                                               sizeof(msg)));
    uint8_t sig[64];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(anchor_priv, msg, sizeof(msg), sig));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(KAT_SIG_PERMANENT, sig, 64);

    /* And the pinned signature verifies through the primitive under test. */
    TEST_ASSERT_TRUE(identity_endorsement_verify(
        anchor_pub, KAT_NODE_PUB, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT, KAT_SIG_PERMANENT));
}

/* --- Tamper cases: every one must reject -------------------------------- */

void test_endorsement_rejects_flipped_node_pubkey_bit(void) {
    uint8_t anchor_pub[32], anchor_priv[64];
    kat_anchor_keypair(anchor_pub, anchor_priv);
    uint8_t bad_pub[32];
    memcpy(bad_pub, KAT_NODE_PUB, 32);
    bad_pub[5] ^= 0x01;
    TEST_ASSERT_FALSE(identity_endorsement_verify(
        anchor_pub, bad_pub, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT, KAT_SIG_PERMANENT));
}

void test_endorsement_rejects_different_not_after(void) {
    uint8_t anchor_pub[32], anchor_priv[64];
    kat_anchor_keypair(anchor_pub, anchor_priv);
    /* Same signature, but the "no cert" sentinel and an arbitrary window must
     * both fail: not_after is inside the signed message. */
    TEST_ASSERT_FALSE(identity_endorsement_verify(
        anchor_pub, KAT_NODE_PUB, IDENTITY_ENDORSEMENT_NOT_AFTER_NONE, KAT_SIG_PERMANENT));
    TEST_ASSERT_FALSE(identity_endorsement_verify(anchor_pub, KAT_NODE_PUB, 0x0102030405060708ULL,
                                                  KAT_SIG_PERMANENT));
}

void test_endorsement_rejects_flipped_sig_bit(void) {
    uint8_t anchor_pub[32], anchor_priv[64];
    kat_anchor_keypair(anchor_pub, anchor_priv);
    uint8_t bad_sig[64];
    memcpy(bad_sig, KAT_SIG_PERMANENT, 64);
    bad_sig[10] ^= 0x08;
    TEST_ASSERT_FALSE(identity_endorsement_verify(
        anchor_pub, KAT_NODE_PUB, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT, bad_sig));
}

void test_endorsement_rejects_wrong_anchor(void) {
    uint8_t wrong_pub[32], wrong_priv[64];
    /* A different anchor seed => a different anchor key => must reject. */
    uint8_t seed[32];
    memcpy(seed, KAT_ANCHOR_SEED, 32);
    seed[0] ^= 0xff;
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(seed, wrong_pub, wrong_priv));
    TEST_ASSERT_FALSE(identity_endorsement_verify(
        wrong_pub, KAT_NODE_PUB, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT, KAT_SIG_PERMANENT));
}

/* --- Anchor provisioning: set/get/is_set/fingerprint + persistence ------- */

void test_anchor_unset_by_default(void) {
    TEST_ASSERT_FALSE(identity_anchor_is_set());
    uint8_t out[32];
    memset(out, 0x5a, sizeof(out));
    TEST_ASSERT_EQUAL(-1, identity_anchor_get(out)); /* fail-closed */
    for (size_t i = 0; i < sizeof(out); i++)
        TEST_ASSERT_EQUAL_UINT8(0x5a, out[i]); /* untouched on failure */
}

void test_anchor_unset_fingerprint_is_zero_sentinel(void) {
    uint8_t fp[4];
    memset(fp, 0x5a, sizeof(fp));
    identity_anchor_fingerprint(fp);
    const uint8_t zero[4] = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_MEMORY(zero, fp, 4);
}

void test_anchor_set_then_get_roundtrip(void) {
    uint8_t pub[32];
    for (int i = 0; i < 32; i++)
        pub[i] = (uint8_t)(0x80 + i);
    TEST_ASSERT_EQUAL(0, identity_anchor_set(pub));
    TEST_ASSERT_TRUE(identity_anchor_is_set());
    uint8_t out[32];
    TEST_ASSERT_EQUAL(0, identity_anchor_get(out));
    TEST_ASSERT_EQUAL_MEMORY(pub, out, 32);
}

/* Fingerprint == SHA256(anchor_pub)[0:4] for a known pubkey (input 00..1f):
 * SHA256 = 630dcd29..., so the first four bytes are 63 0d cd 29 (this is the
 * same known SHA256 pinned in test_identity.c). */
void test_anchor_fingerprint_matches_sha256_prefix(void) {
    uint8_t pub[32];
    for (int i = 0; i < 32; i++)
        pub[i] = (uint8_t)i;
    TEST_ASSERT_EQUAL(0, identity_anchor_set(pub));
    uint8_t fp[4];
    identity_anchor_fingerprint(fp);
    const uint8_t want[4] = {0x63, 0x0d, 0xcd, 0x29};
    TEST_ASSERT_EQUAL_MEMORY(want, fp, 4);
}

void test_anchor_persists_across_reboot(void) {
    uint8_t pub[32];
    for (int i = 0; i < 32; i++)
        pub[i] = (uint8_t)(0xC0 ^ i);
    TEST_ASSERT_EQUAL(0, identity_anchor_set(pub)); /* writes the blob store */

    identity_anchor_clear(); /* simulate reboot: in-memory only, store intact */
    TEST_ASSERT_FALSE(identity_anchor_is_set());

    TEST_ASSERT_EQUAL(0, identity_anchor_load());
    TEST_ASSERT_TRUE(identity_anchor_is_set());
    uint8_t out[32];
    TEST_ASSERT_EQUAL(0, identity_anchor_get(out));
    TEST_ASSERT_EQUAL_MEMORY(pub, out, 32);
}

void test_anchor_load_finds_nothing_on_fresh_flash(void) {
    /* setUp reset the store and cleared memory. */
    TEST_ASSERT_EQUAL(-1, identity_anchor_load());
    TEST_ASSERT_FALSE(identity_anchor_is_set());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_endorsement_msg_exact_layout);
    RUN_TEST(test_endorsement_msg_permanent_sentinel_layout);
    RUN_TEST(test_endorsement_msg_rejects_short_buffer);
    RUN_TEST(test_endorsement_kat_permanent_verifies);
    RUN_TEST(test_endorsement_rejects_flipped_node_pubkey_bit);
    RUN_TEST(test_endorsement_rejects_different_not_after);
    RUN_TEST(test_endorsement_rejects_flipped_sig_bit);
    RUN_TEST(test_endorsement_rejects_wrong_anchor);
    RUN_TEST(test_anchor_unset_by_default);
    RUN_TEST(test_anchor_unset_fingerprint_is_zero_sentinel);
    RUN_TEST(test_anchor_set_then_get_roundtrip);
    RUN_TEST(test_anchor_fingerprint_matches_sha256_prefix);
    RUN_TEST(test_anchor_persists_across_reboot);
    RUN_TEST(test_anchor_load_finds_nothing_on_fresh_flash);
    return UNITY_END();
}
