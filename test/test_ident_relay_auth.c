/*
 * Identity attestation relay-gate MAC (per-node identity Phase 3, Part A).
 *
 * The 158-byte attestation frame carries TWO independent authenticators:
 *   - sig: Ed25519 over the canonical message ("bramble-ident-v1"...),
 *     the identity claim's TRUTH (self-authenticating, keyless-verifiable;
 *     pinned by test_identity_attestation.c, unchanged from Phase 2);
 *   - auth_hmac: the network-key MAC (label "bramble-ident-relay-v1")
 *     that gates RELAY PRIVILEGE, preserving the branch invariant that
 *     keyless traffic never propagates. Relays check this CHEAP MAC first
 *     and never run the expensive Ed25519 verify.
 *
 * This file pins the MAC's exact coverage:
 *   src_addr(4, BE) || x25519_pub(32) || ed25519_pub(32) || sig(64)
 *                   || seq(6)
 * and its exact EXCLUSION of the header (hop_limit is relay-mutable; a
 * relay decrements it and passes the frame through otherwise UNMODIFIED,
 * so the MAC must survive the hop unchanged). Every tamper test is
 * non-vacuous: the untampered control verifies first.
 */
#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "network_key.h"
#include "routing_auth.h"
#include "packet.h"

#include "../components/crypto/crypto_host.c"
#include "../components/network_key/network_key.c"
#include "../components/packet/packet.c"
#include "../components/routing_auth/routing_auth.c"

#include <string.h>

void setUp(void) { network_key_clear(); }
void tearDown(void) { network_key_clear(); }

/* Fully populated, Ed25519-signed, relay-gate-MACed attestation: the exact
 * frame send_identity_attestation (main/mesh_task.c) originates. */
static void make_signed_attestation(bramble_identity_attestation_t* p, uint8_t sk[64]) {
    memset(p, 0, sizeof(*p));
    p->header.version = BRAMBLE_VERSION;
    p->header.type = PKT_TYPE_IDENTITY_ATTESTATION;
    p->header.flags = 0;
    p->header.hop_limit = 8;
    p->header.dest_addr = 0xFFFFFFFFu;
    p->header.packet_id = 0x0BADC0DEu;
    p->src_addr = 0xAABBCCDDu;
    for (int i = 0; i < 32; i++)
        p->x25519_pub[i] = (uint8_t)(0x40 + i);
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair(p->ed25519_pub, sk));

    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(p, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(sk, msg, sizeof(msg), p->sig));

    /* Origin seq (mesh_task.c draws it via control_seq_next); the MAC is
     * computed AFTER the Ed25519 sign because sig is MAC-covered. */
    p->seq[0] = 0x00;
    p->seq[1] = 0x01;
    p->seq[2] = 0x02;
    p->seq[3] = 0x03;
    p->seq[4] = 0x04;
    p->seq[5] = 0x05;
    ident_relay_sign(p);
}

/* Control: a freshly signed frame verifies (unprovisioned PSK-fallback
 * network key, the same default every other control-plane MAC test uses). */
static void test_good_mac_verifies(void) {
    bramble_identity_attestation_t p;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_signed_attestation(&p, sk);
    TEST_ASSERT_TRUE(ident_relay_verify(&p));
}

/* MAC survives a full wire round trip: what a relay deserializes verifies
 * exactly as the originator signed it. */
static void test_mac_survives_wire_round_trip(void) {
    bramble_identity_attestation_t p, rx;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_signed_attestation(&p, sk);

    uint8_t buf[IDENTITY_ATTESTATION_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_serialize(&p, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_deserialize(&rx, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(ident_relay_verify(&rx));
}

/* Tamper harness: control verifies, then one covered field flips and
 * verification must fail. */
static void tamper_and_expect_fail(void (*mutate)(bramble_identity_attestation_t*)) {
    bramble_identity_attestation_t p;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_signed_attestation(&p, sk);
    TEST_ASSERT_TRUE(ident_relay_verify(&p)); /* non-vacuous control */
    mutate(&p);
    TEST_ASSERT_FALSE(ident_relay_verify(&p));
}

static void mutate_src_addr(bramble_identity_attestation_t* p) { p->src_addr ^= 1u; }
static void mutate_x25519(bramble_identity_attestation_t* p) { p->x25519_pub[31] ^= 0x01; }
static void mutate_ed25519(bramble_identity_attestation_t* p) { p->ed25519_pub[0] ^= 0x01; }
static void mutate_sig(bramble_identity_attestation_t* p) { p->sig[63] ^= 0x01; }
static void mutate_seq(bramble_identity_attestation_t* p) { p->seq[5] ^= 0x01; }
static void mutate_mac(bramble_identity_attestation_t* p) { p->auth_hmac[0] ^= 0x01; }

static void test_tampered_src_addr_fails(void) { tamper_and_expect_fail(mutate_src_addr); }
static void test_tampered_x25519_fails(void) { tamper_and_expect_fail(mutate_x25519); }
static void test_tampered_ed25519_fails(void) { tamper_and_expect_fail(mutate_ed25519); }
static void test_tampered_sig_fails(void) { tamper_and_expect_fail(mutate_sig); }
static void test_tampered_seq_fails(void) { tamper_and_expect_fail(mutate_seq); }
static void test_tampered_mac_fails(void) { tamper_and_expect_fail(mutate_mac); }

/* Header exclusion: every header field a relay or the packet layer touches
 * (hop_limit decrement, per-send packet_id, flags, dest) mutates WITHOUT
 * invalidating the MAC. This is the property that lets a relay pass the
 * frame through unmodified except hop_limit. */
static void test_header_fields_excluded_from_mac(void) {
    bramble_identity_attestation_t p;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_signed_attestation(&p, sk);
    TEST_ASSERT_TRUE(ident_relay_verify(&p));

    p.header.hop_limit = 1;
    p.header.packet_id = 0xDEADBEEFu;
    p.header.flags = 0xFF;
    p.header.dest_addr = 0x12345678u;
    TEST_ASSERT_TRUE(ident_relay_verify(&p));
}

/* Keyless outsider: a frame MACed under a DIFFERENT network key fails
 * verification at a provisioned node (and vice versa). This is the relay
 * gate itself: no network key, no propagation. */
static void test_wrong_network_key_fails(void) {
    bramble_identity_attestation_t p;
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    make_signed_attestation(&p, sk); /* signed under the PSK fallback key */
    TEST_ASSERT_TRUE(ident_relay_verify(&p));

    uint8_t fleet_key[32];
    memset(fleet_key, 0x77, sizeof(fleet_key));
    network_key_set_provisioned(fleet_key);
    TEST_ASSERT_FALSE(ident_relay_verify(&p));

    /* Re-signed under the provisioned key it verifies again (control). */
    ident_relay_sign(&p);
    TEST_ASSERT_TRUE(ident_relay_verify(&p));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_good_mac_verifies);
    RUN_TEST(test_mac_survives_wire_round_trip);
    RUN_TEST(test_tampered_src_addr_fails);
    RUN_TEST(test_tampered_x25519_fails);
    RUN_TEST(test_tampered_ed25519_fails);
    RUN_TEST(test_tampered_sig_fails);
    RUN_TEST(test_tampered_seq_fails);
    RUN_TEST(test_tampered_mac_fails);
    RUN_TEST(test_header_fields_excluded_from_mac);
    RUN_TEST(test_wrong_network_key_fails);
    return UNITY_END();
}
