#include "unity.h"
#include "../components/crypto/crypto_host.c"
#include "../components/channel/channel_key.c"

void setUp(void) {}
void tearDown(void) {}

void test_deterministic_derivation(void) {
    bramble_channel_t ch1, ch2;
    TEST_ASSERT_EQUAL(0, channel_derive_key("my-secret-psk", &ch1));
    TEST_ASSERT_EQUAL(0, channel_derive_key("my-secret-psk", &ch2));
    TEST_ASSERT_EQUAL_MEMORY(ch1.key, ch2.key, 32);
    TEST_ASSERT_EQUAL(ch1.channel_id, ch2.channel_id);
}

void test_different_psk_different_key(void) {
    bramble_channel_t ch1, ch2;
    channel_derive_key("psk-alpha", &ch1);
    channel_derive_key("psk-beta", &ch2);
    TEST_ASSERT_FALSE(memcmp(ch1.key, ch2.key, 32) == 0);
}

void test_channel_id_from_hash(void) {
    bramble_channel_t ch;
    channel_derive_key("test", &ch);
    TEST_ASSERT_TRUE(ch.channel_id < 16);
}

void test_epoch_advance_changes_key(void) {
    bramble_channel_t ch;
    channel_derive_key("psk", &ch);
    uint8_t orig[32];
    memcpy(orig, ch.key, 32);
    TEST_ASSERT_EQUAL(0, channel_advance_epoch(&ch));
    TEST_ASSERT_EQUAL(1, ch.epoch);
    TEST_ASSERT_FALSE(memcmp(orig, ch.key, 32) == 0);
}

void test_epoch_one_way(void) {
    /* Can't derive previous key from current */
    bramble_channel_t ch;
    channel_derive_key("psk", &ch);
    uint8_t epoch0[32];
    memcpy(epoch0, ch.key, 32);
    channel_advance_epoch(&ch);
    /* epoch1 key is different from epoch0 — already tested.
       We just verify it's not trivially reversible by checking
       HKDF with epoch0 info doesn't give back epoch0 key */
    uint8_t check[32];
    uint8_t info[2] = {0, 0}; /* epoch 0 info */
    crypto_hkdf_sha256((const uint8_t *)"bramble-channel-epoch", 21,
                       ch.key, 32, info, 2, check, 32);
    TEST_ASSERT_FALSE(memcmp(check, epoch0, 32) == 0);
}

void test_catchup_convergence(void) {
    /* Two channels from same PSK, one advances 5 epochs,
       other catches up — they converge */
    bramble_channel_t ch1, ch2;
    channel_derive_key("shared", &ch1);
    channel_derive_key("shared", &ch2);
    for (int i = 0; i < 5; i++) channel_advance_epoch(&ch1);
    for (int i = 0; i < 5; i++) channel_advance_epoch(&ch2);
    TEST_ASSERT_EQUAL_MEMORY(ch1.key, ch2.key, 32);
    TEST_ASSERT_EQUAL(ch1.epoch, ch2.epoch);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_deterministic_derivation);
    RUN_TEST(test_different_psk_different_key);
    RUN_TEST(test_channel_id_from_hash);
    RUN_TEST(test_epoch_advance_changes_key);
    RUN_TEST(test_epoch_one_way);
    RUN_TEST(test_catchup_convergence);
    return UNITY_END();
}
