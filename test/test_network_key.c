#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "network_key.h"

#include "../components/crypto/crypto_host.c"
#include "../components/network_key/network_key.c"

/*
 * Task 3.1 (PART 3, staged, not closed): the minimal network key provider.
 * These tests exercise the mechanism (domain separation, stability,
 * provisioned/unprovisioned toggling) in isolation. They say nothing about
 * whether SEC-H1/H2/NEW-SEC-4/NEW-SEC-8 are closed: they are not, until
 * real key provisioning replaces the PSK fallback tested here.
 */

void setUp(void) { network_key_clear(); }
void tearDown(void) {}

void test_domain_separation_different_labels_yield_different_macs(void) {
    const uint8_t data[] = "identical data over the wire";
    uint8_t mac_rrep[8];
    uint8_t mac_rerr[8];
    network_key_mac("bramble-rrep-v2", data, sizeof(data), mac_rrep);
    network_key_mac("bramble-rerr-v2", data, sizeof(data), mac_rerr);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(mac_rrep, mac_rerr, sizeof(mac_rrep)));
}

void test_mac_stable_for_same_label_and_data(void) {
    const uint8_t data[] = "same data, same label";
    uint8_t mac1[8];
    uint8_t mac2[8];
    network_key_mac("bramble-ack-v2", data, sizeof(data), mac1);
    network_key_mac("bramble-ack-v2", data, sizeof(data), mac2);
    TEST_ASSERT_EQUAL_MEMORY(mac1, mac2, sizeof(mac1));
}

void test_is_provisioned_flips_after_set_and_clear(void) {
    TEST_ASSERT_EQUAL(0, network_key_is_provisioned());
    uint8_t key[32];
    crypto_random(key, 32);
    network_key_set_provisioned(key);
    TEST_ASSERT_EQUAL(1, network_key_is_provisioned());
    network_key_clear();
    TEST_ASSERT_EQUAL(0, network_key_is_provisioned());
}

void test_network_key_get_returns_provisioned_key_when_set(void) {
    uint8_t key[32];
    crypto_random(key, 32);
    network_key_set_provisioned(key);
    uint8_t out[32];
    TEST_ASSERT_EQUAL(0, network_key_get(out));
    TEST_ASSERT_EQUAL_MEMORY(key, out, sizeof(key));
}

/* The unprovisioned fallback must be deterministic (same PSK in, same key
 * out every time) so every node derives the identical fallback and control-
 * plane MACs still verify against each other while unprovisioned. */
void test_network_key_get_fallback_is_deterministic(void) {
    uint8_t out1[32];
    uint8_t out2[32];
    TEST_ASSERT_EQUAL(0, network_key_get(out1));
    TEST_ASSERT_EQUAL(0, network_key_get(out2));
    TEST_ASSERT_EQUAL_MEMORY(out1, out2, sizeof(out1));
}

/* Switching from unprovisioned to provisioned must actually change the
 * derived key material used for MACs, not just the is_provisioned flag. */
void test_mac_changes_between_unprovisioned_and_provisioned(void) {
    const uint8_t data[] = "control plane body";
    uint8_t mac_fallback[8];
    network_key_mac("bramble-beacon-v2", data, sizeof(data), mac_fallback);

    uint8_t key[32];
    crypto_random(key, 32);
    network_key_set_provisioned(key);

    uint8_t mac_provisioned[8];
    network_key_mac("bramble-beacon-v2", data, sizeof(data), mac_provisioned);

    TEST_ASSERT_NOT_EQUAL(0, memcmp(mac_fallback, mac_provisioned, sizeof(mac_fallback)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_domain_separation_different_labels_yield_different_macs);
    RUN_TEST(test_mac_stable_for_same_label_and_data);
    RUN_TEST(test_is_provisioned_flips_after_set_and_clear);
    RUN_TEST(test_network_key_get_returns_provisioned_key_when_set);
    RUN_TEST(test_network_key_get_fallback_is_deterministic);
    RUN_TEST(test_mac_changes_between_unprovisioned_and_provisioned);
    return UNITY_END();
}
