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

/*
 * Task 3.3 / SEC-H1 (STAGED, not closed: see network_key.h). rerr_sign/
 * rerr_verify authenticate broken_dest||broken_next_hop, the 2
 * origin-stable RERR fields, while deliberately excluding reporter_addr
 * and header.packet_id: mesh_task.c's send_rerr rebuilds a fresh struct
 * with its OWN reporter_addr and a fresh packet_id on every call
 * (original detection AND every re-origination), passing broken_dest/
 * broken_next_hop through unchanged from whoever called it. Confirmed by
 * reading send_rerr directly before writing any code here.
 */

void setUp(void) { network_key_clear(); }
void tearDown(void) {}

static bramble_rerr_t make_rerr(uint32_t reporter_addr, uint32_t packet_id) {
    bramble_rerr_t r = {0};
    r.header.version = BRAMBLE_VERSION;
    r.header.type = PKT_TYPE_RERR;
    r.header.hop_limit = 8;
    r.header.dest_addr = 0xFFFFFFFF;
    r.header.packet_id = packet_id;
    r.reporter_addr = reporter_addr;
    r.broken_dest = 0xAAAAAAAA;
    r.broken_next_hop = 0xBBBBBBBB;
    return r;
}

void test_rerr_sign_verify_round_trip(void) {
    bramble_rerr_t r = make_rerr(0x11111111, 0x1000);
    rerr_sign(&r);
    TEST_ASSERT_TRUE(rerr_verify(&r));
}

void test_rerr_verify_rejects_tampered_broken_dest(void) {
    bramble_rerr_t r = make_rerr(0x11111111, 0x1000);
    rerr_sign(&r);
    r.broken_dest ^= 0xFFFFFFFF; /* tamper an origin-stable field after signing */
    TEST_ASSERT_FALSE(rerr_verify(&r));
}

/*
 * The key regression case. A forwarder re-originating (send_rerr's exact
 * per-hop behavior) carries its own reporter_addr and a fresh packet_id.
 * This isolates the field-exclusion property directly: the ORIGINAL
 * auth_hmac, carried through unchanged onto a struct with a different
 * reporter_addr/packet_id, must still verify.
 */
void test_rerr_verify_survives_reorigination(void) {
    bramble_rerr_t r = make_rerr(0x11111111, 0x1000);
    rerr_sign(&r);

    bramble_rerr_t reoriginated = r;
    reoriginated.reporter_addr = 0x22222222;
    reoriginated.header.packet_id = 0x2000;
    /* Sanity: the fields actually differ, or this test would pass
     * vacuously without exercising anything. */
    TEST_ASSERT_NOT_EQUAL(r.reporter_addr, reoriginated.reporter_addr);
    TEST_ASSERT_NOT_EQUAL(r.header.packet_id, reoriginated.header.packet_id);

    TEST_ASSERT_TRUE(rerr_verify(&reoriginated));
}

void test_rerr_verify_rejects_wrong_key_forgery(void) {
    bramble_rerr_t r = make_rerr(0x11111111, 0x1000);

    uint8_t attacker_key[32];
    crypto_random(attacker_key, 32);
    network_key_set_provisioned(attacker_key);
    rerr_sign(&r);

    network_key_clear();
    TEST_ASSERT_FALSE(rerr_verify(&r));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rerr_sign_verify_round_trip);
    RUN_TEST(test_rerr_verify_rejects_tampered_broken_dest);
    RUN_TEST(test_rerr_verify_survives_reorigination);
    RUN_TEST(test_rerr_verify_rejects_wrong_key_forgery);
    return UNITY_END();
}
