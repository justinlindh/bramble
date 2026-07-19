#include "unity.h"
#include <openssl/rand.h>
#include "../components/crypto/crypto_host.c"
#include "../components/identity/identity.c"

void setUp(void) {}
void tearDown(void) {}

void test_collision_same_addr_different_hash(void) {
    bramble_identity_t me;
    crypto_generate_identity(&me);
    /* Same address, different pubkey_hash → collision */
    TEST_ASSERT_TRUE(identity_check_collision(&me, me.address, me.pubkey_hash ^ 1));
}

void test_no_collision_same_identity(void) {
    bramble_identity_t me;
    crypto_generate_identity(&me);
    /* Same address, same hash → our own beacon, not a collision */
    TEST_ASSERT_FALSE(identity_check_collision(&me, me.address, me.pubkey_hash));
}

void test_no_collision_different_addr(void) {
    bramble_identity_t me;
    crypto_generate_identity(&me);
    /* Different address → no collision regardless */
    TEST_ASSERT_FALSE(identity_check_collision(&me, me.address ^ 1, me.pubkey_hash ^ 1));
}

/* Pin the address/pubkey_hash derivation spec to exact bytes so BOTH backends
 * are held to the same definition. For an input key of 00 01 02 ... 1f:
 *   SHA256 = 630dcd29 66c43366 91125448 bbb25b4f f412a49c 732db2c8 abc1b858 1bd710dd
 *   address     = SHA256[0:4] = 0x630DCD29
 *   pubkey_hash = SHA256[4:8] = 0x66C43366  (independent slice, NOT the address)
 * The DERIVATION FUNCTION is unchanged by the Phase 4 rebind: only the input
 * key changed (Ed25519 identity public key instead of the X25519 one), which
 * the identity-level tests below pin. The device backend (crypto_esp.c)
 * historically returned the address as the pubkey_hash, which made
 * identity_check_collision a no-op on device; these constants pin the fix on
 * both sides. */
void test_pubkey_hash_pinned_to_independent_slice(void) {
    uint8_t pk[BRAMBLE_KEY_SIZE];
    for (int i = 0; i < BRAMBLE_KEY_SIZE; i++)
        pk[i] = (uint8_t)i;
    TEST_ASSERT_EQUAL_HEX32(0x630DCD29u, crypto_derive_address(pk));
    TEST_ASSERT_EQUAL_HEX32(0x66C43366u, crypto_derive_pubkey_hash(pk));
    TEST_ASSERT_NOT_EQUAL(crypto_derive_address(pk), crypto_derive_pubkey_hash(pk));
}

void test_generated_identity_hash_distinct_from_address(void) {
    bramble_identity_t me;
    TEST_ASSERT_EQUAL(0, crypto_generate_identity(&me));
    TEST_ASSERT_EQUAL_HEX32(crypto_derive_address(me.ed25519_public_key), me.address);
    TEST_ASSERT_EQUAL_HEX32(crypto_derive_pubkey_hash(me.ed25519_public_key), me.pubkey_hash);
    TEST_ASSERT_NOT_EQUAL(me.address, me.pubkey_hash);
}

/* Phase 1 gave every identity a working Ed25519 keypair; Phase 4 rebinds the
 * address to it. */
void test_generated_identity_has_working_ed25519_keypair(void) {
    bramble_identity_t me;
    TEST_ASSERT_EQUAL(0, crypto_generate_identity(&me));

    /* Sign with the identity's Ed25519 private key, verify with its public. */
    const uint8_t msg[] = "bramble identity phase 1";
    uint8_t sig[BRAMBLE_ED25519_SIG_SIZE];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(me.ed25519_private_key, msg, sizeof(msg), sig));
    TEST_ASSERT_TRUE(crypto_ed25519_verify(me.ed25519_public_key, msg, sizeof(msg), sig));
    /* Wrong message must not verify (real keypair, not garbage bytes). */
    TEST_ASSERT_FALSE(crypto_ed25519_verify(me.ed25519_public_key, msg, sizeof(msg) - 1, sig));

    /* libsodium secret-key layout: seed(32) || public(32). */
    TEST_ASSERT_EQUAL_MEMORY(me.ed25519_public_key, me.ed25519_private_key + 32,
                             BRAMBLE_ED25519_PUBKEY_SIZE);

    /* THE PHASE 4 REBIND: the address derives from the Ed25519 identity
     * public key, and NOT from the X25519 key anymore. This is what makes
     * an attested address unforgeable: claiming an address means exhibiting
     * a valid signature under the exact key the address hashes from. */
    TEST_ASSERT_EQUAL_HEX32(crypto_derive_address(me.ed25519_public_key), me.address);
    TEST_ASSERT_NOT_EQUAL(crypto_derive_address(me.public_key), me.address);
}

void test_two_identities_have_distinct_ed25519_keys(void) {
    bramble_identity_t a, b;
    TEST_ASSERT_EQUAL(0, crypto_generate_identity(&a));
    TEST_ASSERT_EQUAL(0, crypto_generate_identity(&b));
    TEST_ASSERT_TRUE(
        memcmp(a.ed25519_public_key, b.ed25519_public_key, BRAMBLE_ED25519_PUBKEY_SIZE) != 0);
}

/* --- Persistence (identity_save / identity_load) ---------------------------
 * These run against the host blob-store backend of identity.c; the save/load
 * and migration logic itself is shared with the device (NVS) backend. */

void test_identity_load_fails_on_empty_store(void) {
    identity_host_store_reset();
    bramble_identity_t id;
    TEST_ASSERT_EQUAL(-1, identity_load(&id));
}

void test_identity_save_load_roundtrips_all_keys(void) {
    identity_host_store_reset();
    bramble_identity_t saved, loaded;
    TEST_ASSERT_EQUAL(0, crypto_generate_identity(&saved));
    TEST_ASSERT_EQUAL(0, identity_save(&saved));

    memset(&loaded, 0, sizeof(loaded));
    TEST_ASSERT_EQUAL(0, identity_load(&loaded));
    TEST_ASSERT_EQUAL_MEMORY(saved.private_key, loaded.private_key, BRAMBLE_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(saved.public_key, loaded.public_key, BRAMBLE_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(saved.ed25519_public_key, loaded.ed25519_public_key,
                             BRAMBLE_ED25519_PUBKEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(saved.ed25519_private_key, loaded.ed25519_private_key,
                             BRAMBLE_ED25519_SECKEY_SIZE);
    TEST_ASSERT_EQUAL_HEX32(saved.address, loaded.address);
    TEST_ASSERT_EQUAL_HEX32(saved.pubkey_hash, loaded.pubkey_hash);

    /* The reloaded Ed25519 keypair still signs/verifies. */
    const uint8_t msg[] = "roundtrip";
    uint8_t sig[BRAMBLE_ED25519_SIG_SIZE];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(loaded.ed25519_private_key, msg, sizeof(msg), sig));
    TEST_ASSERT_TRUE(crypto_ed25519_verify(loaded.ed25519_public_key, msg, sizeof(msg), sig));
}

/* Migration: a pre-Phase-1 store holds only the X25519 blobs. identity_load
 * must keep the existing X25519 identity, generate a fresh Ed25519 keypair
 * for it, and persist that keypair so the next load returns the same Ed
 * keys. THE FLAG DAY (Phase 4, owner-approved, pre-alpha): the address is
 * Ed25519-derived, so an upgrading node gets a NEW address; the old
 * X25519-derived address is gone. Peers' stale pins for the old address
 * are RAM-only and simply age out. */
void test_identity_migration_from_x25519_only_store(void) {
    identity_host_store_reset();
    bramble_identity_t old_id;
    TEST_ASSERT_EQUAL(0, crypto_generate_identity(&old_id));
    /* Simulate the old on-flash layout: only "priv"/"pub" blobs present. */
    TEST_ASSERT_EQUAL(0, id_store_write("priv", old_id.private_key, BRAMBLE_KEY_SIZE));
    TEST_ASSERT_EQUAL(0, id_store_write("pub", old_id.public_key, BRAMBLE_KEY_SIZE));

    bramble_identity_t migrated;
    memset(&migrated, 0, sizeof(migrated));
    TEST_ASSERT_EQUAL(0, identity_load(&migrated));

    /* X25519 keys are preserved; the address and pubkey_hash now derive
     * from the freshly generated Ed25519 key (the flag day: NOT the old
     * X25519-derived address). */
    TEST_ASSERT_EQUAL_MEMORY(old_id.private_key, migrated.private_key, BRAMBLE_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(old_id.public_key, migrated.public_key, BRAMBLE_KEY_SIZE);
    TEST_ASSERT_EQUAL_HEX32(crypto_derive_address(migrated.ed25519_public_key), migrated.address);
    TEST_ASSERT_EQUAL_HEX32(crypto_derive_pubkey_hash(migrated.ed25519_public_key),
                            migrated.pubkey_hash);
    TEST_ASSERT_NOT_EQUAL(crypto_derive_address(old_id.public_key), migrated.address);

    /* A working Ed25519 keypair was generated for the old identity... */
    const uint8_t msg[] = "migrated";
    uint8_t sig[BRAMBLE_ED25519_SIG_SIZE];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(migrated.ed25519_private_key, msg, sizeof(msg), sig));
    TEST_ASSERT_TRUE(crypto_ed25519_verify(migrated.ed25519_public_key, msg, sizeof(msg), sig));

    /* ...and persisted: a second load returns the SAME Ed keys, not fresh
     * ones. */
    bramble_identity_t again;
    memset(&again, 0, sizeof(again));
    TEST_ASSERT_EQUAL(0, identity_load(&again));
    TEST_ASSERT_EQUAL_MEMORY(migrated.ed25519_public_key, again.ed25519_public_key,
                             BRAMBLE_ED25519_PUBKEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(migrated.ed25519_private_key, again.ed25519_private_key,
                             BRAMBLE_ED25519_SECKEY_SIZE);
}

/* --- Fail-closed contract for crypto_generate_identity (issue #71) --------
 * The header promises *id is left byte-for-byte untouched on ANY failure.
 * Force the RNG to fail so the function takes an error path, then assert the
 * caller's buffer really is untouched. Before the fix the X25519 private and
 * public keys were written into *id before the Ed25519 step could fail, so a
 * caller that ignored the return value got live-looking key material. */
/* Fail the Nth and every later RAND draw, delegating earlier ones to the real
 * RNG. Sweeping N walks the failure point through the function: N == 0 fails
 * the X25519 keygen (nothing written yet), while a later N lets the X25519 key
 * succeed and fails the Ed25519 seed draw, which is the partial-write path
 * where the pre-fix code had already stored real key material in *id. Sweeping
 * rather than hardcoding an N keeps the test robust to how many RAND calls
 * OpenSSL makes internally. */
static const RAND_METHOD* g_real_rand;
static int g_rand_calls;
static int g_rand_fail_after;

static int counting_rand_bytes(unsigned char* buf, int num) {
    if (g_rand_calls++ >= g_rand_fail_after)
        return 0; /* 0 = failure for a RAND_METHOD */
    return g_real_rand->bytes(buf, num);
}
static int counting_rand_status(void) { return 1; }

void test_generate_identity_leaves_id_untouched_on_rng_failure(void) {
/* RAND_set_rand_method is deprecated in OpenSSL 3 but is still the only
 * portable seam for forcing RAND_bytes to fail from a test. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    static RAND_METHOD counting_method = {
        NULL, counting_rand_bytes, NULL, NULL, counting_rand_bytes, counting_rand_status,
    };
    g_real_rand = RAND_get_rand_method();

    int saw_failure = 0;
    for (int fail_after = 0; fail_after <= 8; fail_after++) {
        bramble_identity_t id;
        memset(&id, 0xA5, sizeof(id));
        bramble_identity_t before;
        memcpy(&before, &id, sizeof(id));

        g_rand_calls = 0;
        g_rand_fail_after = fail_after;
        RAND_set_rand_method(&counting_method);
        int rc = crypto_generate_identity(&id);
        RAND_set_rand_method(g_real_rand);

        if (rc != 0) {
            saw_failure = 1;
            /* The contract: a failed call leaves the caller's buffer exactly
             * as it found it, so no key material (whole or partial) escapes. */
            TEST_ASSERT_EQUAL_MEMORY(&before, &id, sizeof(id));
        }
    }
#pragma GCC diagnostic pop

    /* Guard against the injection silently not working and the loop above
     * asserting nothing. */
    TEST_ASSERT_TRUE_MESSAGE(saw_failure, "RNG fault injection never induced a failure");
}

void test_generate_identity_overwrites_every_field_on_success(void) {
    /* The commit is a single whole-struct copy, so nothing stale survives a
     * successful call into the caller's buffer. */
    bramble_identity_t id;
    memset(&id, 0xA5, sizeof(id));
    TEST_ASSERT_EQUAL(0, crypto_generate_identity(&id));

    bramble_identity_t poison;
    memset(&poison, 0xA5, sizeof(poison));
    TEST_ASSERT_NOT_EQUAL(
        0, memcmp(id.private_key, poison.private_key, sizeof(id.private_key)));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(id.public_key, poison.public_key, sizeof(id.public_key)));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(id.ed25519_private_key, poison.ed25519_private_key,
                                    sizeof(id.ed25519_private_key)));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(id.ed25519_public_key, poison.ed25519_public_key,
                                    sizeof(id.ed25519_public_key)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_generate_identity_leaves_id_untouched_on_rng_failure);
    RUN_TEST(test_generate_identity_overwrites_every_field_on_success);
    RUN_TEST(test_collision_same_addr_different_hash);
    RUN_TEST(test_no_collision_same_identity);
    RUN_TEST(test_no_collision_different_addr);
    RUN_TEST(test_pubkey_hash_pinned_to_independent_slice);
    RUN_TEST(test_generated_identity_hash_distinct_from_address);
    RUN_TEST(test_generated_identity_has_working_ed25519_keypair);
    RUN_TEST(test_two_identities_have_distinct_ed25519_keys);
    RUN_TEST(test_identity_load_fails_on_empty_store);
    RUN_TEST(test_identity_save_load_roundtrips_all_keys);
    RUN_TEST(test_identity_migration_from_x25519_only_store);
    return UNITY_END();
}
