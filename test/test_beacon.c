#include "unity.h"
#include "network_key.h"
#include "../components/routing/beacon.c"
#include "../components/packet/packet.c"
#include "../components/crypto/crypto_host.c"
#include "../components/network_key/network_key.c"

void setUp(void) { network_key_clear(); }
void tearDown(void) {}

void test_beacon_build(void) {
    bramble_beacon_t b = beacon_build(0xAABBCCDD, 0x11223344, 120, 85, 3, 5, 0x01, 1000000, 500);
    TEST_ASSERT_EQUAL(BRAMBLE_VERSION, b.header.version);
    TEST_ASSERT_EQUAL(PKT_TYPE_BEACON, b.header.type);
    TEST_ASSERT_EQUAL(0xFFFFFFFF, b.header.dest_addr);
    TEST_ASSERT_EQUAL(0xAABBCCDD, b.src_addr);
    TEST_ASSERT_EQUAL(0x11223344, b.pubkey_hash);
    TEST_ASSERT_EQUAL(120, b.uptime_min);
    TEST_ASSERT_EQUAL(85, b.battery_pct);
    TEST_ASSERT_EQUAL(3, b.tx_queue_depth);
    TEST_ASSERT_EQUAL(5, b.neighbor_count);
    TEST_ASSERT_EQUAL(0x01, b.flags);
    TEST_ASSERT_EQUAL(1000000, b.network_time);
    TEST_ASSERT_EQUAL(500, b.time_confidence);
}

void test_beacon_hmac_compute_verify(void) {
    uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    beacon_compute_hmac(&b, key, sizeof(key));
    /* HMAC should be non-zero */
    uint8_t zero[4] = {0};
    TEST_ASSERT_FALSE(memcmp(b.auth_hmac, zero, 4) == 0);
    /* Verify should pass */
    TEST_ASSERT_TRUE(beacon_verify_hmac(&b, key, sizeof(key)));
}

void test_beacon_hmac_wrong_key(void) {
    uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t bad_key[16] = {99, 98, 97, 96, 95, 94, 93, 92, 91, 90, 89, 88, 87, 86, 85, 84};
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    beacon_compute_hmac(&b, key, sizeof(key));
    TEST_ASSERT_FALSE(beacon_verify_hmac(&b, bad_key, sizeof(bad_key)));
}

void test_beacon_hmac_zero_key(void) {
    uint8_t key[16] = {0};
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    beacon_compute_hmac(&b, key, sizeof(key));
    TEST_ASSERT_TRUE(beacon_verify_hmac(&b, key, sizeof(key)));
}

/*
 * SEC-H2 (Task 3.4, STAGED, not closed: see network_key.h). Mirrors
 * mesh_task.c's provisioned beacon key derivation exactly: HKDF over
 * network_key_get()'s output with label "bramble-beacon-v2", producing
 * the labeled subkey fed into beacon_compute_hmac/verify_hmac as
 * s_beacon_key. beacon_verify_hmac's constant-time-ness is a code-review
 * property (an OR-accumulate compare with no early exit, replacing the
 * old memcmp), not something a functional unit test can observe via
 * timing; this suite verifies it functionally (accept the right key,
 * reject the wrong one) alongside the source change itself.
 */
static void derive_beacon_key_from_network_key(const uint8_t net_key[32], uint8_t beacon_key[32]) {
    const char* salt = "bramble-beacon-v2";
    TEST_ASSERT_EQUAL(0, crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), net_key, 32, NULL,
                                            0, beacon_key, 32));
}

void test_beacon_hmac_under_provisioned_network_key_verifies(void) {
    uint8_t net_key[32];
    crypto_random(net_key, 32);
    network_key_set_provisioned(net_key);

    uint8_t net_key_out[32];
    TEST_ASSERT_EQUAL(0, network_key_get(net_key_out));
    uint8_t beacon_key[32];
    derive_beacon_key_from_network_key(net_key_out, beacon_key);

    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    beacon_compute_hmac(&b, beacon_key, sizeof(beacon_key));
    TEST_ASSERT_TRUE(beacon_verify_hmac(&b, beacon_key, sizeof(beacon_key)));
}

/* Genuinely discriminating: proves the two network keys derive DIFFERENT
 * beacon subkeys (else the reject assertion below would pass vacuously
 * for the trivial reason that the "different" key wasn't actually
 * different), then confirms verification under the wrong subkey rejects. */
void test_beacon_hmac_rejects_different_network_key(void) {
    uint8_t net_key_a[32];
    uint8_t net_key_b[32];
    crypto_random(net_key_a, 32);
    crypto_random(net_key_b, 32);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(net_key_a, net_key_b, sizeof(net_key_a)));

    uint8_t beacon_key_a[32];
    uint8_t beacon_key_b[32];
    derive_beacon_key_from_network_key(net_key_a, beacon_key_a);
    derive_beacon_key_from_network_key(net_key_b, beacon_key_b);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(beacon_key_a, beacon_key_b, sizeof(beacon_key_a)));

    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    beacon_compute_hmac(&b, beacon_key_a, sizeof(beacon_key_a));
    TEST_ASSERT_FALSE(beacon_verify_hmac(&b, beacon_key_b, sizeof(beacon_key_b)));
}

/* Fix 4 (red-team panel): beacon_compute_hmac previously covered only the
 * fixed 32 bytes before auth_hmac; the optional name (serialized AFTER
 * auth_hmac, at BEACON_SIZE) was outside the HMAC entirely, so an attacker
 * could rewrite any captured beacon's name and it would still verify,
 * spoofing peer names in the neighbor table/UI even under a provisioned
 * key. This proves the name is now covered: tampering it must break
 * verify. */
void test_beacon_hmac_covers_name_tamper_rejected(void) {
    uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    b.name_len = 5;
    memcpy(b.name, "alice", 5);
    b.name[5] = '\0';
    beacon_compute_hmac(&b, key, sizeof(key));
    TEST_ASSERT_TRUE(beacon_verify_hmac(&b, key, sizeof(key))); /* untampered: verifies */

    bramble_beacon_t tampered = b;
    memcpy(tampered.name, "mallo", 5); /* same length, different content */
    TEST_ASSERT_FALSE(beacon_verify_hmac(&tampered, key, sizeof(key)));
}

/* A beacon with no name at all (name_len == 0, the common case) must still
 * compute and verify correctly: the fix must not require a name to be
 * present. */
void test_beacon_hmac_still_verifies_with_no_name(void) {
    uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    TEST_ASSERT_EQUAL(0, b.name_len);
    beacon_compute_hmac(&b, key, sizeof(key));
    TEST_ASSERT_TRUE(beacon_verify_hmac(&b, key, sizeof(key)));
}

/*
 * ws 1.3b: the 48-bit seq lives INSIDE the fixed prefix beacon_compute_hmac
 * hashes (before auth_hmac, per packet.h), so it must be covered
 * automatically without any beacon.c change: prefix_len is derived from
 * BEACON_SIZE - sizeof(auth_hmac), not a hardcoded byte count, so growing
 * BEACON_SIZE to fit seq grows the covered prefix too. This proves it:
 * tampering seq after signing must break verification exactly like
 * tampering the name does above.
 */
void test_beacon_seq_covered_by_hmac(void) {
    uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    b.seq[0] = 0x01;
    b.seq[1] = 0x02;
    b.seq[2] = 0x03;
    b.seq[3] = 0x04;
    b.seq[4] = 0x05;
    b.seq[5] = 0x06;
    beacon_compute_hmac(&b, key, sizeof(key));
    TEST_ASSERT_TRUE(
        beacon_verify_hmac(&b, key, sizeof(key))); /* sanity: correctly signed with seq */

    bramble_beacon_t tampered = b;
    tampered.seq[5] ^= 0xFF; /* tamper the seq after signing */
    TEST_ASSERT_FALSE(beacon_verify_hmac(&tampered, key, sizeof(key)));
}

/*
 * Red-team fix regression. send_beacon's TX buffer (main/mesh_beacon.c) is
 * not host-testable (ESP-IDF only, board-build-verified), but the size
 * invariant it depends on lives entirely in the host-testable serialize
 * path: BEACON_SIZE + 1 + BEACON_NAME_MAX is the wire size of a beacon
 * with a full-length name, and that must be exactly the size that fits.
 * Before this fix send_beacon used a hand-counted buf[64], 7 bytes short
 * of this, so a 10+ character name made bramble_beacon_serialize fail
 * with ESP_ERR_INVALID_SIZE and the node silently stopped beaconing
 * entirely (no neighbor announce, mailbox flush, or timesync) until the
 * name was cleared.
 */
void test_beacon_serialize_max_name_fits_size_invariant(void) {
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    b.name_len = BEACON_NAME_MAX;
    memset(b.name, 'x', BEACON_NAME_MAX);
    b.name[BEACON_NAME_MAX] = '\0';

    uint8_t buf[BEACON_SIZE + 1 + BEACON_NAME_MAX];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_beacon_serialize(&b, buf, sizeof(buf)));

    /* One byte short of the invariant must fail: this is exactly the gap
     * the old buf[64] fell into (64 < 71). */
    uint8_t too_small[BEACON_SIZE + BEACON_NAME_MAX];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      bramble_beacon_serialize(&b, too_small, sizeof(too_small)));
}

/*
 * A node name may run to BRAMBLE_NODE_NAME_MAX (32) bytes while a beacon
 * carries at most BEACON_NAME_MAX (16), so a name gets cut for the air.
 * Cutting on a raw byte count splits whatever multi-byte character straddles
 * byte 16, and the beacon then carries a partial UTF-8 sequence that every
 * neighbour stores and renders as a replacement character. The cut has to
 * land on a character boundary instead.
 */
void test_utf8_trunc_len_cuts_on_character_boundaries(void) {
    /* Pure ASCII: the cut is the budget, nothing to pull back. */
    TEST_ASSERT_EQUAL(16, bramble_utf8_trunc_len((const uint8_t*)"aaaaaaaaaaaaaaaaaaaa", 20, 16));

    /* Shorter than the budget: returned unchanged. */
    TEST_ASSERT_EQUAL(3, bramble_utf8_trunc_len((const uint8_t*)"abc", 3, 16));

    /* Eight 2-byte characters land exactly on 16. */
    const char* accents = "ééééééééé"; /* 9 chars, 18 bytes */
    TEST_ASSERT_EQUAL(16, bramble_utf8_trunc_len((const uint8_t*)accents, 18, 16));

    /* Six 3-byte characters are 18 bytes, so five fit and the cut is 15,
     * not the 16 a raw byte count would take (splitting the sixth). */
    const char* cjk = "日本語日本語"; /* 6 chars, 18 bytes */
    TEST_ASSERT_EQUAL(15, bramble_utf8_trunc_len((const uint8_t*)cjk, 18, 16));

    /* Four 4-byte characters are 16 bytes exactly; five would be 20. */
    const char* trees = "🌲🌲🌲🌲🌲"; /* 5 chars, 20 bytes */
    TEST_ASSERT_EQUAL(16, bramble_utf8_trunc_len((const uint8_t*)trees, 20, 16));

    /* One ASCII byte then 4-byte characters: 'a' + three trees is 13, and a
     * fourth tree would need 17, so the cut is 13 rather than 16. */
    const char* mixed = "a🌲🌲🌲🌲";
    TEST_ASSERT_EQUAL(13, bramble_utf8_trunc_len((const uint8_t*)mixed, 17, 16));

    /* Not UTF-8 at all: still capped, not emptied. */
    const uint8_t junk[20] = {0xFF, 0xFE, 0xFF, 0xFE, 0xFF, 0xFE, 0xFF, 0xFE, 0xFF, 0xFE,
                              0xFF, 0xFE, 0xFF, 0xFE, 0xFF, 0xFE, 0xFF, 0xFE, 0xFF, 0xFE};
    TEST_ASSERT_EQUAL(16, bramble_utf8_trunc_len(junk, sizeof(junk), 16));

    TEST_ASSERT_EQUAL(0, bramble_utf8_trunc_len(NULL, 4, 16));
    TEST_ASSERT_EQUAL(0, bramble_utf8_trunc_len((const uint8_t*)trees, 20, 3));
}

/*
 * The serializer is the last gate before the air, so it must apply the same
 * boundary rule even when a caller hands it an over-long name_len, and
 * bramble_beacon_wire_size must agree byte for byte: beacon_compute_hmac
 * derives its HMAC input length from wire_size, so any disagreement between
 * the two would authenticate a different span than it transmits.
 */
void test_beacon_serialize_never_emits_a_split_character(void) {
    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    /* Six 3-byte characters: 18 bytes, over both the field and the cap. */
    const char* cjk = "日本語日本語";
    b.name_len = BEACON_NAME_MAX; /* a caller that already clamped on bytes */
    memcpy(b.name, cjk, BEACON_NAME_MAX);
    b.name[BEACON_NAME_MAX] = '\0';

    uint8_t buf[BEACON_SIZE + 1 + BEACON_NAME_MAX];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_beacon_serialize(&b, buf, sizeof(buf)));

    /* Five characters survive, the sixth is dropped whole. */
    TEST_ASSERT_EQUAL(15, buf[BEACON_SIZE]);
    TEST_ASSERT_EQUAL_MEMORY(cjk, buf + BEACON_SIZE + 1, 15);
    TEST_ASSERT_EQUAL(BEACON_SIZE + 1 + 15, bramble_beacon_wire_size(&b));

    /* What went on the wire must be well-formed on its own terms: running the
     * boundary scan over the emitted bytes at their own length has nothing
     * left to trim. (Checking the final byte is not a continuation byte would
     * be wrong, since the last byte of any multi-byte character is one.) */
    TEST_ASSERT_EQUAL(15, bramble_utf8_trunc_len(buf + BEACON_SIZE + 1, 15, 15));

    bramble_beacon_t out;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_beacon_deserialize(&out, buf, bramble_beacon_wire_size(&b)));
    TEST_ASSERT_EQUAL(15, out.name_len);
    TEST_ASSERT_EQUAL_MEMORY(cjk, out.name, 15);
}

/* The HMAC covers the name region, and its length comes from wire_size, so
 * a truncated multi-byte name must still verify end to end. */
void test_beacon_hmac_verifies_over_a_boundary_truncated_name(void) {
    uint8_t key[32];
    memset(key, 0x5A, sizeof(key));

    bramble_beacon_t b = beacon_build(0xAABB, 0x1234, 10, 90, 1, 2, 0, 5000, 100);
    b.name_len = BEACON_NAME_MAX;
    memcpy(b.name, "日本語日本語", BEACON_NAME_MAX);
    b.name[BEACON_NAME_MAX] = '\0';

    beacon_compute_hmac(&b, key, sizeof(key));
    TEST_ASSERT_TRUE(beacon_verify_hmac(&b, key, sizeof(key)));
}

/*
 * The protocol documents battery_pct=0xFF as "unknown/plugged in"
 * (docs/bramble-protocol-spec.md), emitted by main/mesh_beacon.c whenever
 * battery_beacon_pct() (components/battery, unit-tested in test_battery.c)
 * decides the node is confirmed charging. beacon_build itself has no
 * charging awareness: it is a plain field passthrough, so this proves 0xFF
 * survives that passthrough and the wire serialize/deserialize round trip
 * intact, exactly like any other battery_pct value (test_beacon_build
 * already covers 85; this is the sentinel's own value, not a special case
 * in the wire format).
 */
void test_beacon_battery_pct_sentinel_255_round_trips(void) {
    bramble_beacon_t b = beacon_build(0xAABBCCDD, 0x11223344, 120, 0xFF, 3, 5, 0x01, 1000000, 500);
    TEST_ASSERT_EQUAL_UINT8(0xFF, b.battery_pct);

    uint8_t buf[BEACON_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_beacon_serialize(&b, buf, sizeof(buf)));

    bramble_beacon_t out = {0};
    TEST_ASSERT_EQUAL(ESP_OK, bramble_beacon_deserialize(&out, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT8(0xFF, out.battery_pct);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_beacon_build);
    RUN_TEST(test_beacon_battery_pct_sentinel_255_round_trips);
    RUN_TEST(test_beacon_hmac_compute_verify);
    RUN_TEST(test_beacon_hmac_wrong_key);
    RUN_TEST(test_beacon_hmac_zero_key);
    RUN_TEST(test_beacon_hmac_under_provisioned_network_key_verifies);
    RUN_TEST(test_beacon_hmac_rejects_different_network_key);
    RUN_TEST(test_beacon_hmac_covers_name_tamper_rejected);
    RUN_TEST(test_beacon_hmac_still_verifies_with_no_name);
    RUN_TEST(test_beacon_seq_covered_by_hmac);
    RUN_TEST(test_beacon_serialize_max_name_fits_size_invariant);
    RUN_TEST(test_utf8_trunc_len_cuts_on_character_boundaries);
    RUN_TEST(test_beacon_serialize_never_emits_a_split_character);
    RUN_TEST(test_beacon_hmac_verifies_over_a_boundary_truncated_name);
    return UNITY_END();
}
