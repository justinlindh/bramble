#include "unity.h"
#include "dm_session.h"
#include "../components/crypto/crypto_host.c"
#include "../components/dm_session/dm_session.c"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_quad_dh_both_sides_agree(void) {
    bramble_identity_t a_id, b_id, a_eph, b_eph;
    crypto_generate_identity(&a_id); crypto_generate_identity(&b_id);
    crypto_generate_identity(&a_eph); crypto_generate_identity(&b_eph);
    uint32_t lo = a_id.address < b_id.address ? a_id.address : b_id.address;
    uint32_t hi = a_id.address < b_id.address ? b_id.address : a_id.address;

    uint8_t ka[32], kb[32];
    TEST_ASSERT_EQUAL(0, dm_derive_session_key(a_id.private_key, a_eph.private_key,
        b_id.public_key, b_eph.public_key, lo, hi, 0, ka));
    TEST_ASSERT_EQUAL(0, dm_derive_session_key(b_id.private_key, b_eph.private_key,
        a_id.public_key, a_eph.public_key, lo, hi, 0, kb));
    TEST_ASSERT_EQUAL_MEMORY(ka, kb, 32);
}

void test_different_epoch_yields_different_key(void) {
    bramble_identity_t a_id, b_id, a_eph, b_eph;
    crypto_generate_identity(&a_id); crypto_generate_identity(&b_id);
    crypto_generate_identity(&a_eph); crypto_generate_identity(&b_eph);
    uint8_t k0[32], k1[32];
    dm_derive_session_key(a_id.private_key, a_eph.private_key, b_id.public_key, b_eph.public_key,
                          a_id.address, b_id.address, 0, k0);
    dm_derive_session_key(a_id.private_key, a_eph.private_key, b_id.public_key, b_eph.public_key,
                          a_id.address, b_id.address, 1, k1);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(k0, k1, 32));
}

/* RFC 7748 low-order points: for ANY scalar these force an all-zero shared
 * secret. All-zero (u=0) is the trivial case OpenSSL already rejects upstream,
 * so it proves nothing about our added guard. Use a NON-trivial low-order
 * u-coordinate (order 8, little-endian) so the vector is real; the accumulator
 * guard is what protects the mbedtls DEVICE path, which does not reject these. */
static const uint8_t k_low_order_u[32] = {
    0xe0,0xeb,0x7a,0x7c,0x3b,0x41,0xb8,0xae,0x16,0x56,0xe3,0xfa,0xf1,0x9f,0xc4,0x6a,
    0xda,0x09,0x8d,0xeb,0x9c,0x32,0xb1,0xfd,0x86,0x62,0x05,0x16,0x5f,0x49,0xb8,0x00
};
void test_low_order_dh_rejected(void) {
    uint8_t priv[32]; crypto_random(priv, 32);
    uint8_t out[32];
    /* Contract: crypto_x25519_dh must fail on a low-order peer point. On host
     * OpenSSL rejects it upstream; on device only the added accumulator guard
     * catches it. This asserts the CONTRACT, not the red phase (see note below). */
    TEST_ASSERT_NOT_EQUAL(0, crypto_x25519_dh(priv, k_low_order_u, out));
}
/* Red-phase / device-path proof: the added guard is `acc |= ss[i]; if(!acc)
 * return -1;`. Because it is inline in crypto_x25519_dh and the host primitive
 * short-circuits first, this host test cannot exercise the guard directly.
 * Prove it against the real target with a shared-secret-level assertion: */
void test_zero_shared_secret_guard(void) {
    /* Simulate what the mbedtls path hands back for a low-order point: an
     * all-zero shared secret. crypto_x25519_check_shared (extracted guard,
     * added below) must reject it. */
    uint8_t zero_ss[32] = {0};
    uint8_t ok_ss[32]; crypto_random(ok_ss, 32); ok_ss[0] |= 1;
    TEST_ASSERT_NOT_EQUAL(0, crypto_x25519_check_shared(zero_ss));
    TEST_ASSERT_EQUAL(0, crypto_x25519_check_shared(ok_ss));
}

/* Fix 1: pin ct_le32's correctness (not just its timing). ct_le32 is a
 * static helper in dm_session.c, visible here via the direct source
 * #include above (host-test convention). Big-endian: a[0] is most
 * significant, so a difference there must dominate a later byte even if
 * every later byte of the "smaller" array is larger. */
void test_ct_le32_matches_lexicographic_order(void) {
    uint8_t a[32], b[32];

    /* Differ only in the last (least significant) byte. */
    memset(a, 0x00, sizeof(a)); a[31] = 0x01;
    memset(b, 0x00, sizeof(b)); b[31] = 0x02;
    TEST_ASSERT_EQUAL(1, ct_le32(a, b));
    TEST_ASSERT_EQUAL(0, ct_le32(b, a));

    /* Differ only in the first (most significant) byte, with every other
     * byte pointing the "wrong" way: a's tail is all 0xFF, b's tail is all
     * 0x00. The first-byte difference must still decide it. */
    memset(a, 0xFF, sizeof(a)); a[0] = 0x01;
    memset(b, 0x00, sizeof(b)); b[0] = 0x02;
    TEST_ASSERT_EQUAL(1, ct_le32(a, b));
    TEST_ASSERT_EQUAL(0, ct_le32(b, a));

    /* Equal arrays: a tie counts as <=. */
    memset(a, 0x42, sizeof(a));
    memset(b, 0x42, sizeof(b));
    TEST_ASSERT_EQUAL(1, ct_le32(a, b));
    TEST_ASSERT_EQUAL(1, ct_le32(b, a));
}

/* Fix 1: SAS known-vector pin. Expected value computed once offline with
 * the exact HKDF-SHA256(salt="bramble-sas", ikm, info=NULL) call
 * dm_derive_sas makes, over ikm[i] = i for i in 0..127. */
void test_sas_known_vector(void) {
    uint8_t ikm[128];
    for (int i = 0; i < 128; i++) ikm[i] = (uint8_t)i;

    char sas[8];
    TEST_ASSERT_EQUAL(0, dm_derive_sas(ikm, sas));
    TEST_ASSERT_EQUAL_STRING("2805852", sas);
    TEST_ASSERT_EQUAL(7, strlen(sas));
}

/* Fix 1: both parties, deriving the SAS from their own independently
 * computed (but equal, per test_quad_dh_both_sides_agree) 128-byte IKM,
 * must land on the same human-comparable string. dm_compute_ikm is a
 * static helper in dm_session.c (visible via the direct #include), used
 * here only to capture the raw IKM each side derives; dm_derive_session_key
 * itself is unchanged. */
void test_sas_both_sides_agree(void) {
    bramble_identity_t a_id, b_id, a_eph, b_eph;
    crypto_generate_identity(&a_id); crypto_generate_identity(&b_id);
    crypto_generate_identity(&a_eph); crypto_generate_identity(&b_eph);

    uint8_t ikm_a[128], ikm_b[128];
    TEST_ASSERT_EQUAL(0, dm_compute_ikm(a_id.private_key, a_eph.private_key,
        b_id.public_key, b_eph.public_key, ikm_a));
    TEST_ASSERT_EQUAL(0, dm_compute_ikm(b_id.private_key, b_eph.private_key,
        a_id.public_key, a_eph.public_key, ikm_b));
    TEST_ASSERT_EQUAL_MEMORY(ikm_a, ikm_b, 128); /* same underlying fix as 0.1.1's key agreement */

    char sas_a[8], sas_b[8];
    TEST_ASSERT_EQUAL(0, dm_derive_sas(ikm_a, sas_a));
    TEST_ASSERT_EQUAL(0, dm_derive_sas(ikm_b, sas_b));
    TEST_ASSERT_EQUAL_STRING(sas_a, sas_b);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_quad_dh_both_sides_agree);
    RUN_TEST(test_different_epoch_yields_different_key);
    RUN_TEST(test_low_order_dh_rejected);
    RUN_TEST(test_zero_shared_secret_guard);
    RUN_TEST(test_ct_le32_matches_lexicographic_order);
    RUN_TEST(test_sas_known_vector);
    RUN_TEST(test_sas_both_sides_agree);
    return UNITY_END();
}
