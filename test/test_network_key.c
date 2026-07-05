#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "network_key.h"

#include "../components/crypto/crypto_host.c"
#include "../components/network_key/network_key.c"

/*
 * Fail-closed network key provider (mandatory-provisioning campaign, Task 1).
 * There is NO public-PSK fallback: an unprovisioned node has NO usable key,
 * emits NO valid MAC, and reports the all-zero fingerprint sentinel. These
 * tests pin that contract. Task 2 wires the control plane to check
 * network_key_mac's return value.
 */

void setUp(void) { network_key_clear(); }
void tearDown(void) {}

/* ── Fail-closed: unprovisioned means NO key material ─────────────────── */

void test_unprovisioned_get_fails_and_leaves_key_out_untouched(void) {
    uint8_t out[32];
    memset(out, 0x5A, sizeof(out)); /* sentinel: must survive a failed get */
    TEST_ASSERT_NOT_EQUAL(0, network_key_get(out));
    for (size_t i = 0; i < sizeof(out); i++) {
        TEST_ASSERT_EQUAL_UINT8(0x5A, out[i]); /* non-vacuous: nothing written */
    }
}

void test_unprovisioned_fingerprint_is_all_zero_sentinel(void) {
    uint8_t fp[4];
    memset(fp, 0x5A, sizeof(fp));
    network_key_fingerprint(fp);
    const uint8_t zero[4] = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_MEMORY(zero, fp, sizeof(fp));
}

void test_unprovisioned_mac_fails_closed(void) {
    const uint8_t data[] = "control plane body";
    uint8_t out[8];
    memset(out, 0x5A, sizeof(out));
    int rc = network_key_mac("bramble-rrep-v2", data, sizeof(data), out);
    TEST_ASSERT_NOT_EQUAL(0, rc); /* signals unprovisioned to the caller */
    const uint8_t zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_MEMORY(zero, out, sizeof(out)); /* no MAC over a zero key */
}

/* ── Explicit provisioning and clear (NOT a fallback) ────────────────── */

void test_set_provisioned_then_get_returns_that_key(void) {
    uint8_t key[32];
    crypto_random(key, 32);
    network_key_set_provisioned(key);
    TEST_ASSERT_EQUAL(1, network_key_is_provisioned());
    uint8_t out[32];
    TEST_ASSERT_EQUAL(0, network_key_get(out));
    TEST_ASSERT_EQUAL_MEMORY(key, out, sizeof(key));
}

void test_clear_reverts_to_fail_closed_not_fallback(void) {
    uint8_t key[32];
    crypto_random(key, 32);
    network_key_set_provisioned(key);
    network_key_clear();
    TEST_ASSERT_EQUAL(0, network_key_is_provisioned());
    uint8_t out[32];
    memset(out, 0x5A, sizeof(out));
    TEST_ASSERT_NOT_EQUAL(0, network_key_get(out)); /* fails, does not fall back */
    for (size_t i = 0; i < sizeof(out); i++) {
        TEST_ASSERT_EQUAL_UINT8(0x5A, out[i]);
    }
}

/* ── fingerprint: stable and convergent for provisioned keys ─────────── */

void test_fingerprint_stable_key_dependent_and_convergent(void) {
    uint8_t key[32];
    memset(key, 0xAB, sizeof(key));
    network_key_set_provisioned(key);
    uint8_t fp1[4];
    uint8_t fp2[4];
    network_key_fingerprint(fp1);
    network_key_fingerprint(fp2);
    TEST_ASSERT_EQUAL_MEMORY(fp1, fp2, 4); /* stable for a fixed key */

    /* A different key yields a different fingerprint. */
    network_key_clear();
    uint8_t key_b[32];
    memset(key_b, 0xCD, sizeof(key_b));
    network_key_set_provisioned(key_b);
    uint8_t fp_b[4];
    network_key_fingerprint(fp_b);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(fp1, fp_b, 4));

    /* Convergence: an identical key (as on another node) matches. */
    network_key_clear();
    uint8_t key_same[32];
    memset(key_same, 0xAB, sizeof(key_same));
    network_key_set_provisioned(key_same);
    uint8_t fp_same[4];
    network_key_fingerprint(fp_same);
    TEST_ASSERT_EQUAL_MEMORY(fp1, fp_same, 4);
}

/* ── MAC domain separation and stability (provisioned) ───────────────── */

void test_domain_separation_different_labels_yield_different_macs(void) {
    uint8_t key[32];
    crypto_random(key, 32);
    network_key_set_provisioned(key);
    const uint8_t data[] = "identical data over the wire";
    uint8_t mac_rrep[8];
    uint8_t mac_rerr[8];
    TEST_ASSERT_EQUAL(0, network_key_mac("bramble-rrep-v2", data, sizeof(data), mac_rrep));
    TEST_ASSERT_EQUAL(0, network_key_mac("bramble-rerr-v2", data, sizeof(data), mac_rerr));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(mac_rrep, mac_rerr, sizeof(mac_rrep)));
}

void test_mac_stable_for_same_label_and_data(void) {
    uint8_t key[32];
    crypto_random(key, 32);
    network_key_set_provisioned(key);
    const uint8_t data[] = "same data, same label";
    uint8_t mac1[8];
    uint8_t mac2[8];
    TEST_ASSERT_EQUAL(0, network_key_mac("bramble-ack-v2", data, sizeof(data), mac1));
    TEST_ASSERT_EQUAL(0, network_key_mac("bramble-ack-v2", data, sizeof(data), mac2));
    TEST_ASSERT_EQUAL_MEMORY(mac1, mac2, sizeof(mac1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_unprovisioned_get_fails_and_leaves_key_out_untouched);
    RUN_TEST(test_unprovisioned_fingerprint_is_all_zero_sentinel);
    RUN_TEST(test_unprovisioned_mac_fails_closed);
    RUN_TEST(test_set_provisioned_then_get_returns_that_key);
    RUN_TEST(test_clear_reverts_to_fail_closed_not_fallback);
    RUN_TEST(test_fingerprint_stable_key_dependent_and_convergent);
    RUN_TEST(test_domain_separation_different_labels_yield_different_macs);
    RUN_TEST(test_mac_stable_for_same_label_and_data);
    return UNITY_END();
}
