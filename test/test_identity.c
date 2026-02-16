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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_collision_same_addr_different_hash);
    RUN_TEST(test_no_collision_same_identity);
    RUN_TEST(test_no_collision_different_addr);
    return UNITY_END();
}
