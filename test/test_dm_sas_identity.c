#include "unity.h"
#include "dm_session.h"
#include "../components/crypto/crypto_host.c"
#include "../components/dm_session/dm_session.c"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Order independence: (a,b) and (b,a) produce the same SAS. */
void test_identity_sas_order_independent(void) {
    uint8_t ka[32], kb[32];
    for (int i = 0; i < 32; i++) {
        ka[i] = (uint8_t)i;
        kb[i] = (uint8_t)(255 - i);
    }
    char s1[8], s2[8];
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(ka, kb, 0x1111, 0x2222, s1));
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(kb, ka, 0x2222, 0x1111, s2));
    TEST_ASSERT_EQUAL_STRING(s1, s2);
    TEST_ASSERT_EQUAL(7, strlen(s1));
}

/* Stability across sessions: same identity keys -> same SAS regardless of any
 * session/ephemeral state (the point of the identity-bound redefinition). */
void test_identity_sas_stable_across_sessions(void) {
    uint8_t ka[32], kb[32];
    for (int i = 0; i < 32; i++) {
        ka[i] = (uint8_t)(i * 3);
        kb[i] = (uint8_t)(i * 7 + 1);
    }
    char s1[8], s2[8];
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(ka, kb, 0xAAAA, 0xBBBB, s1));
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(ka, kb, 0xAAAA, 0xBBBB, s2));
    TEST_ASSERT_EQUAL_STRING(s1, s2);
}

/* Stability is independent of the session IKM / ratchet state: derive the SAS,
 * then spin up a full ratchet session from unrelated ephemerals and derive it
 * again from the same identity keys. The two MUST match: the SAS commits to the
 * pinned identity keys alone, not the session. */
void test_identity_sas_independent_of_session_state(void) {
    bramble_identity_t a, b, a_eph, b_eph;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);

    char before[8];
    TEST_ASSERT_EQUAL(
        0, dm_derive_identity_sas(a.public_key, b.public_key, a.address, b.address, before));

    /* Establish a live ratchet session (mutates session_key, chains, epoch). */
    uint8_t ikm[128];
    TEST_ASSERT_EQUAL(
        0, dm_compute_ikm(a.private_key, a_eph.private_key, b.public_key, b_eph.public_key, ikm));
    dm_session_t s;
    memset(&s, 0, sizeof(s));
    s.peer_addr = b.address;
    s.state = DM_STATE_ACTIVE;
    s.ke_epoch = 7;
    dm_session_ratchet_init_state(&s, ikm, a.address, b.address);

    char after[8];
    TEST_ASSERT_EQUAL(
        0, dm_derive_identity_sas(a.public_key, b.public_key, a.address, b.address, after));
    TEST_ASSERT_EQUAL_STRING(before, after);
}

/* MitM detection: a different peer identity key -> different SAS. */
void test_identity_sas_detects_key_substitution(void) {
    uint8_t ka[32], kb[32], kb_mitm[32];
    for (int i = 0; i < 32; i++) {
        ka[i] = (uint8_t)i;
        kb[i] = (uint8_t)(i + 5);
    }
    memcpy(kb_mitm, kb, 32);
    kb_mitm[0] ^= 0x01;
    char honest[8], mitm[8];
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(ka, kb, 0x1111, 0x2222, honest));
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(ka, kb_mitm, 0x1111, 0x2222, mitm));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(honest, mitm));
}

/* Known vector (fill in Step 4). */
void test_identity_sas_known_vector(void) {
    uint8_t ka[32], kb[32];
    for (int i = 0; i < 32; i++) {
        ka[i] = (uint8_t)i;
        kb[i] = (uint8_t)(128 + i);
    }
    char s[8];
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(ka, kb, 0x1111, 0x2222, s));
    /* Committed KAT: ka[i]=i, kb[i]=128+i, addr_a=0x1111 < addr_b=0x2222 so the
     * canonical order is (ka, kb). Computed once from the reference run. */
    TEST_ASSERT_EQUAL_STRING("3670369", s);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_identity_sas_order_independent);
    RUN_TEST(test_identity_sas_stable_across_sessions);
    RUN_TEST(test_identity_sas_independent_of_session_state);
    RUN_TEST(test_identity_sas_detects_key_substitution);
    RUN_TEST(test_identity_sas_known_vector);
    return UNITY_END();
}
