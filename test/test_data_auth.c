#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "network_key.h"
#include "routing_auth.h"
#include "packet.h"
#include "routing.h"

#include "../components/crypto/crypto_host.c"
#include "../components/network_key/network_key.c"
#include "../components/packet/packet.c"
#include "../components/routing_auth/routing_auth.c"
#include "../components/routing/routing.c"

/*
 * Task 4-fix F1 (Critical): keyless DATA route-poisoning. Wire v4's
 * reverse-route learning installs route(dest=src_addr, next_hop=prev_hop)
 * off every received/forwarded DATA frame. Relays never decrypt DATA, so the
 * AEAD tag cannot gate this; before the fix a keyless attacker (no network
 * key) could inject a DATA frame {src_addr=VICTIM, prev_hop=ATTACKER, ...}
 * and every node in range would install route(VICTIM)->ATTACKER, redirecting
 * or blackholing all traffic to the victim.
 *
 * The fix adds an 8-byte network-key auth_hmac over the origin-stable fields
 * (masked header + src_addr), checked BEFORE the breadcrumb install. These
 * tests exercise data_auth_sign/data_auth_verify directly and reproduce the
 * red-team attack against the actual dispatch gate logic.
 */

void setUp(void) { network_key_clear(); }
void tearDown(void) { network_key_clear(); }

#define VICTIM 0x0A0A0A0Au
#define ATTACKER 0x0E0E0E0Eu
#define SELF 0xAAAAAAAAu

static bramble_header_t make_data_header(uint32_t dest, uint32_t pkt_id) {
    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION;
    h.type = PKT_TYPE_DATA;
    h.flags = FLAG_ENCRYPT | FLAG_CHANNEL;
    h.hop_limit = ROUTE_HOP_LIMIT_MAX;
    h.dest_addr = dest;
    h.packet_id = pkt_id;
    return h;
}

/* Mirrors mesh_process_rx_packet's PKT_TYPE_DATA gate: verify origin auth,
 * and ONLY on success learn the breadcrumb. Returns whether a route was
 * installed. */
static bool dispatch_learns_breadcrumb(routing_table_t* rt, const bramble_header_t* h,
                                       uint32_t src_addr, uint32_t prev_hop,
                                       const uint8_t hmac[8]) {
    if (!data_auth_verify(h, src_addr, hmac))
        return false;
    /* src != self && prev_hop != self && unicast: the data_rx_decide gate. */
    route_install(rt, src_addr, prev_hop, 1, 255, ROUTE_ACTIVE, ROUTE_SRC_BREADCRUMB, 1000);
    return true;
}

/* A legitimately-signed frame verifies and lays its breadcrumb. */
void test_legit_signed_data_verifies_and_learns(void) {
    bramble_header_t h = make_data_header(0xDEADBEEF, 0x11111111);
    uint8_t hmac[8];
    data_auth_sign(&h, VICTIM, hmac);
    TEST_ASSERT_TRUE(data_auth_verify(&h, VICTIM, hmac));

    routing_table_t rt;
    route_init(&rt);
    TEST_ASSERT_TRUE(dispatch_learns_breadcrumb(&rt, &h, VICTIM, 0xB0B0B0B0u, hmac));
    TEST_ASSERT_NOT_NULL(route_lookup(&rt, VICTIM));
}

/* THE ATTACK: a keyless attacker cannot compute the MAC, so whatever garbage
 * it writes into the auth_hmac field fails verification -> NO route installed
 * -> no poisoning. */
void test_f1_keyless_forged_data_installs_no_route(void) {
    bramble_header_t h = make_data_header(0xDEADBEEF, 0x22222222);
    /* Attacker has no key; it just fills the hmac field with junk. */
    uint8_t forged_hmac[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33};

    TEST_ASSERT_FALSE(data_auth_verify(&h, VICTIM, forged_hmac));

    routing_table_t rt;
    route_init(&rt);
    bool learned = dispatch_learns_breadcrumb(&rt, &h, VICTIM, ATTACKER, forged_hmac);
    TEST_ASSERT_FALSE(learned);
    TEST_ASSERT_EQUAL(0, route_count(&rt));
    TEST_ASSERT_NULL(route_lookup(&rt, VICTIM));
}

/* Even an all-zero hmac (the natural "attacker left it blank") is rejected. */
void test_f1_zero_hmac_installs_no_route(void) {
    bramble_header_t h = make_data_header(0xDEADBEEF, 0x33333333);
    uint8_t zero_hmac[8] = {0};
    TEST_ASSERT_FALSE(data_auth_verify(&h, VICTIM, zero_hmac));
}

/* src_addr is MAC-bound: an on-path relay that flips the claimed originator
 * (to redirect a returning confirmation toward a spoofed victim) breaks the
 * MAC, so the tampered frame lays no breadcrumb. */
void test_f1_src_addr_tamper_breaks_mac(void) {
    bramble_header_t h = make_data_header(0xDEADBEEF, 0x44444444);
    uint8_t hmac[8];
    data_auth_sign(&h, VICTIM, hmac); /* signed for VICTIM */
    /* Verifier reads a DIFFERENT src_addr off the wire. */
    TEST_ASSERT_FALSE(data_auth_verify(&h, 0xC0FFEEu, hmac));
}

/* hop_limit is masked out of the MAC, so a relay's legitimate decrement does
 * NOT break verification: the MAC survives every forward hop, like the AEAD
 * tag. Without this, multi-hop delivery would fail for legit traffic. */
void test_hop_limit_decrement_preserves_mac(void) {
    bramble_header_t h = make_data_header(0xDEADBEEF, 0x55555555);
    uint8_t hmac[8];
    data_auth_sign(&h, VICTIM, hmac);
    /* Relay decrements before retransmit. */
    h.hop_limit = ROUTE_HOP_LIMIT_MAX - 3;
    TEST_ASSERT_TRUE(data_auth_verify(&h, VICTIM, hmac));
}

/* A tampered dest_addr / flags / packet_id (all MAC-covered header fields)
 * breaks verification. */
void test_covered_header_fields_are_bound(void) {
    bramble_header_t h = make_data_header(0xDEADBEEF, 0x66666666);
    uint8_t hmac[8];
    data_auth_sign(&h, VICTIM, hmac);

    bramble_header_t tampered = h;
    tampered.dest_addr = 0x99999999;
    TEST_ASSERT_FALSE(data_auth_verify(&tampered, VICTIM, hmac));

    tampered = h;
    tampered.flags ^= FLAG_CHANNEL;
    TEST_ASSERT_FALSE(data_auth_verify(&tampered, VICTIM, hmac));

    tampered = h;
    tampered.packet_id = 0xFFFFFFFFu;
    TEST_ASSERT_FALSE(data_auth_verify(&tampered, VICTIM, hmac));
}

/* When a real per-fleet network key is provisioned, an attacker's MAC forged
 * under the public-PSK fallback no longer verifies: the fix has real teeth
 * once provisioning lands (network_key.h's documented baseline). */
void test_provisioned_key_rejects_psk_forged_mac(void) {
    bramble_header_t h = make_data_header(0xDEADBEEF, 0x77777777);
    /* Attacker signs under the unprovisioned PSK fallback. */
    network_key_clear();
    uint8_t psk_forged[8];
    data_auth_sign(&h, VICTIM, psk_forged);

    /* Fleet is provisioned with a real key the attacker does not have. */
    uint8_t fleet_key[32];
    for (int i = 0; i < 32; i++)
        fleet_key[i] = (uint8_t)(i + 1);
    network_key_set_provisioned(fleet_key);

    TEST_ASSERT_FALSE(data_auth_verify(&h, VICTIM, psk_forged));

    /* A frame signed under the fleet key does verify on the fleet. */
    uint8_t fleet_hmac[8];
    data_auth_sign(&h, VICTIM, fleet_hmac);
    TEST_ASSERT_TRUE(data_auth_verify(&h, VICTIM, fleet_hmac));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_legit_signed_data_verifies_and_learns);
    RUN_TEST(test_f1_keyless_forged_data_installs_no_route);
    RUN_TEST(test_f1_zero_hmac_installs_no_route);
    RUN_TEST(test_f1_src_addr_tamper_breaks_mac);
    RUN_TEST(test_hop_limit_decrement_preserves_mac);
    RUN_TEST(test_covered_header_fields_are_bound);
    RUN_TEST(test_provisioned_key_rejects_psk_forged_mac);
    return UNITY_END();
}
