#include "unity.h"
#include "identity.h"
#include <string.h>

/*
 * RPC auth-token minting behind the SEC-L1 entropy gate (issue #89, item 3).
 *
 * The token used to come straight from esp_fill_random(), which is only fully
 * entropic once an RF subsystem is up: a call site added before RF init would
 * silently mint a guessable credential. It now draws from crypto_random(), the
 * gated source, which means minting CAN FAIL. These tests pin the failure
 * behaviour: nothing is written, nothing is persisted, the caller gets a
 * distinguishable IDENTITY_TOKEN_ERR_ENTROPY (so it can retry rather than
 * treating the node as permanently broken), and a retry after the gate opens
 * succeeds.
 *
 * crypto_random is stubbed here (rather than linking crypto_host.c) precisely
 * so the shut-gate branch is reachable on the host: OpenSSL's RAND_bytes has
 * no gate to shut.
 */

static bool g_rng_available = true;
static int g_rng_calls;

int crypto_random(uint8_t* buf, size_t len) {
    g_rng_calls++;
    if (!g_rng_available) {
        /* Mirror crypto_entropy_fill's fail-closed contract: zero the
         * destination so a caller ignoring the return value cannot install
         * predictable bytes. */
        memset(buf, 0, len);
        return -1;
    }
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(0x10 + i);
    return 0;
}

void crypto_secure_wipe(void* buf, size_t len) { memset(buf, 0, len); }

/* Unused by these tests, present so identity.c links. */
int crypto_sha256(const uint8_t* data, size_t data_len, uint8_t* hash) {
    (void)data;
    (void)data_len;
    memset(hash, 0, 32);
    return 0;
}
uint32_t crypto_derive_address(const uint8_t* public_key) {
    (void)public_key;
    return 0;
}
uint32_t crypto_derive_pubkey_hash(const uint8_t* public_key) {
    (void)public_key;
    return 0;
}
int crypto_generate_identity(bramble_identity_t* id) {
    (void)id;
    return -1;
}
int crypto_ed25519_keypair(uint8_t public_key[BRAMBLE_ED25519_PUBKEY_SIZE],
                           uint8_t private_key[BRAMBLE_ED25519_SECKEY_SIZE]) {
    (void)public_key;
    (void)private_key;
    return -1;
}
bool crypto_ed25519_verify(const uint8_t public_key[BRAMBLE_ED25519_PUBKEY_SIZE],
                           const uint8_t* msg, size_t msg_len,
                           const uint8_t sig[BRAMBLE_ED25519_SIG_SIZE]) {
    (void)public_key;
    (void)msg;
    (void)msg_len;
    (void)sig;
    return false;
}

#include "../components/identity/identity.c"

void setUp(void) {
    g_rng_available = true;
    g_rng_calls = 0;
}
void tearDown(void) {}

/* ── Happy path: shape of the minted token ───────────────────────────── */

void test_mint_produces_32_uppercase_hex_chars(void) {
    char token[64];
    memset(token, 0x5A, sizeof(token));
    TEST_ASSERT_EQUAL_INT(0, identity_mint_ws_auth_token(token, sizeof(token)));
    TEST_ASSERT_EQUAL_UINT32(32u, (uint32_t)strlen(token));
    for (size_t i = 0; i < 32; i++) {
        char c = token[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        TEST_ASSERT_TRUE(hex);
    }
    /* Stub RNG emits 0x10, 0x11, ... so the encoding is checkable exactly. */
    TEST_ASSERT_EQUAL_STRING("101112131415161718191A1B1C1D1E1F", token);
}

void test_mint_rejects_a_too_small_buffer(void) {
    char token[32]; /* one short of the 33 needed for 32 chars + NUL */
    TEST_ASSERT_EQUAL_INT(IDENTITY_TOKEN_ERR_STORE,
                          identity_mint_ws_auth_token(token, sizeof(token)));
    TEST_ASSERT_EQUAL_INT(0, g_rng_calls); /* refused before drawing entropy */
}

/* ── Fail-closed: the entropy gate is shut ───────────────────────────── */

void test_mint_fails_closed_when_entropy_is_unavailable(void) {
    g_rng_available = false;
    char token[64];
    memset(token, 0x5A, sizeof(token));
    int rc = identity_mint_ws_auth_token(token, sizeof(token));

    /* Distinguishable from a store fault: the caller must be able to tell
     * "retry once RF is up" from "NVS is broken, this will not fix itself". */
    TEST_ASSERT_EQUAL_INT(IDENTITY_TOKEN_ERR_ENTROPY, rc);
    TEST_ASSERT_NOT_EQUAL(IDENTITY_TOKEN_ERR_STORE, rc);

    /* No token at all, not a short/zero-derived one. */
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)strlen(token));
}

void test_shut_gate_never_yields_the_all_zero_derived_token(void) {
    /* The concrete hazard: crypto_random zeroes its buffer on failure, so a
     * version that ignored the return value would mint the fixed, publicly
     * known string "00000000000000000000000000000000" on every affected
     * device. Assert that exact string is never produced. */
    g_rng_available = false;
    char token[64];
    memset(token, 0x5A, sizeof(token));
    (void)identity_mint_ws_auth_token(token, sizeof(token));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(token, "00000000000000000000000000000000"));
    TEST_ASSERT_EQUAL_STRING("", token);
}

void test_mint_succeeds_on_retry_once_entropy_becomes_ready(void) {
    /* This is what makes deferral (rather than a hard boot failure) the right
     * answer: the condition is transient. ws_server marks the token pending
     * and retries on the next auth evaluation, by which point RF is up. */
    char token[64];
    g_rng_available = false;
    TEST_ASSERT_EQUAL_INT(IDENTITY_TOKEN_ERR_ENTROPY,
                          identity_mint_ws_auth_token(token, sizeof(token)));
    TEST_ASSERT_EQUAL_STRING("", token);

    g_rng_available = true; /* RF subsystem up, gate opened */
    TEST_ASSERT_EQUAL_INT(0, identity_mint_ws_auth_token(token, sizeof(token)));
    TEST_ASSERT_EQUAL_UINT32(32u, (uint32_t)strlen(token));
}

/* ── The host build has no token store at all ────────────────────────── */

void test_host_ensure_reports_a_store_error_not_an_empty_token(void) {
    /* An empty token is NOT open access anywhere in this codebase; callers
     * must see a negative return and fail closed. */
    char token[64];
    memset(token, 0x5A, sizeof(token));
    TEST_ASSERT_TRUE(identity_ensure_ws_auth_token(token, sizeof(token)) < 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mint_produces_32_uppercase_hex_chars);
    RUN_TEST(test_mint_rejects_a_too_small_buffer);
    RUN_TEST(test_mint_fails_closed_when_entropy_is_unavailable);
    RUN_TEST(test_shut_gate_never_yields_the_all_zero_derived_token);
    RUN_TEST(test_mint_succeeds_on_retry_once_entropy_becomes_ready);
    RUN_TEST(test_host_ensure_reports_a_store_error_not_an_empty_token);
    return UNITY_END();
}
