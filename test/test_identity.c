#include "unity.h"
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
 * are held to the same definition. For public_key = 00 01 02 ... 1f:
 *   SHA256 = 630dcd29 66c43366 91125448 bbb25b4f f412a49c 732db2c8 abc1b858 1bd710dd
 *   address     = SHA256[0:4] = 0x630DCD29
 *   pubkey_hash = SHA256[4:8] = 0x66C43366  (independent slice, NOT the address)
 * The device backend (crypto_esp.c) historically returned the address as the
 * pubkey_hash, which made identity_check_collision a no-op on device; these
 * constants pin the fix on both sides. */
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
    TEST_ASSERT_EQUAL_HEX32(crypto_derive_address(me.public_key), me.address);
    TEST_ASSERT_EQUAL_HEX32(crypto_derive_pubkey_hash(me.public_key), me.pubkey_hash);
    TEST_ASSERT_NOT_EQUAL(me.address, me.pubkey_hash);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_collision_same_addr_different_hash);
    RUN_TEST(test_no_collision_same_identity);
    RUN_TEST(test_no_collision_different_addr);
    RUN_TEST(test_pubkey_hash_pinned_to_independent_slice);
    RUN_TEST(test_generated_identity_hash_distinct_from_address);
    return UNITY_END();
}
