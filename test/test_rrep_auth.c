#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "network_key.h"
#include "discovery.h"

#include "../components/crypto/crypto_host.c"
#include "../components/network_key/network_key.c"
#include "../components/routing/routing.c"
#include "../components/routing/discovery.c"

/*
 * Task 3.2 / SEC-H1 (STAGED, not closed: see network_key.h). rrep_sign/
 * rrep_verify authenticate the 4 origin-stable RREP fields (query_id,
 * src_addr, hop_count, route_metric) while deliberately excluding next_hop
 * and header.dest_addr, the only two fields rrep_forward mutates on each
 * relay hop (confirmed by reading rrep_forward directly: it copies the
 * whole struct then rewrites exactly those two fields). Getting this
 * exclusion wrong in either direction is exactly the class of bug that
 * broke RFC r1 (NEW-SEC-3): sign too much and every legitimate relay hop
 * fails verification; sign too little and a relay can tamper an
 * origin-stable field undetected.
 */

void setUp(void) { network_key_clear(); }
void tearDown(void) {}

/* ws 1.3b: the seq is set by the caller (not rrep_build_destination, which
 * defaults it to zero), mirroring how mesh_task.c writes the 48-bit
 * control_seq_next draw into r.seq before re-signing. */
static void set_seq(bramble_rrep_t *r, uint64_t seq) {
    r->seq[0] = (uint8_t)(seq >> 40);
    r->seq[1] = (uint8_t)(seq >> 32);
    r->seq[2] = (uint8_t)(seq >> 24);
    r->seq[3] = (uint8_t)(seq >> 16);
    r->seq[4] = (uint8_t)(seq >> 8);
    r->seq[5] = (uint8_t)seq;
}

static bramble_rreq_t make_rreq(void) {
    bramble_rreq_t rq = {0};
    rq.header.version = BRAMBLE_VERSION;
    rq.header.type = PKT_TYPE_RREQ;
    rq.header.hop_limit = 4;
    rq.header.dest_addr = 0xFFFFFFFF;
    rq.header.packet_id = 0x11112222;
    rq.query_id = 0xAAAA1111;
    rq.encrypted_source = 0xBEEFCAFE;
    rq.hop_count = 2;
    rq.metric = 200;
    rq.prev_hop = 0x22222222;
    rq.rreq_salt = 0x33334444;
    return rq;
}

void test_rrep_sign_verify_round_trip(void) {
    bramble_rreq_t rq = make_rreq();
    bramble_rrep_t r = rrep_build_destination(&rq, 0xDDDDDDDD);
    set_seq(&r, 0x0102030405);
    rrep_sign(&r);
    TEST_ASSERT_TRUE(rrep_verify(&r));
}

/*
 * ws 1.3b: the 48-bit seq is part of the auth buffer (rrep_build_auth_buf),
 * so tampering it after signing must break verification exactly like
 * tampering route_metric does above.
 */
void test_rrep_seq_covered_by_mac(void) {
    bramble_rreq_t rq = make_rreq();
    bramble_rrep_t r = rrep_build_destination(&rq, 0xDDDDDDDD);
    set_seq(&r, 0x0102030405);
    rrep_sign(&r);
    TEST_ASSERT_TRUE(rrep_verify(&r)); /* sanity: correctly signed with the seq included */

    r.seq[5] ^= 0xFF; /* tamper the seq after signing */
    TEST_ASSERT_FALSE(rrep_verify(&r));
}

void test_rrep_verify_rejects_flipped_route_metric(void) {
    bramble_rreq_t rq = make_rreq();
    bramble_rrep_t r = rrep_build_destination(&rq, 0xDDDDDDDD);
    r.route_metric ^= 0xFF; /* tamper an origin-stable field after signing */
    TEST_ASSERT_FALSE(rrep_verify(&r));
}

/*
 * The key regression case. A legitimate relay hop rewrites next_hop and
 * header.dest_addr exactly the way rrep_forward does; this MUST still
 * verify, or every real relay hop would break route discovery.
 */
void test_rrep_verify_survives_relay_rewrite(void) {
    bramble_rreq_t rq = make_rreq();
    bramble_rrep_t r = rrep_build_destination(&rq, 0xDDDDDDDD);
    set_seq(&r, 0x0102030405);
    rrep_sign(&r);

    bramble_rrep_t fwd = rrep_forward(&r, 0x55555555);
    /* Sanity: the rewrite actually happened, or this test would pass
     * vacuously without exercising anything. */
    TEST_ASSERT_NOT_EQUAL(r.next_hop, fwd.next_hop);
    TEST_ASSERT_NOT_EQUAL(r.header.dest_addr, fwd.header.dest_addr);
    /* seq is origin-stable: rrep_forward carries it through unchanged, same
     * as auth_hmac. */
    TEST_ASSERT_EQUAL_MEMORY(r.seq, fwd.seq, sizeof(r.seq));

    TEST_ASSERT_TRUE(rrep_verify(&fwd));
}

void test_rrep_verify_rejects_wrong_key_forgery(void) {
    bramble_rreq_t rq = make_rreq();
    bramble_rrep_t r = rrep_build_destination(&rq, 0xDDDDDDDD);

    /* Attacker forges a fresh, internally-consistent MAC under a key that
     * is not the one a verifier will use. */
    uint8_t attacker_key[32];
    crypto_random(attacker_key, 32);
    network_key_set_provisioned(attacker_key);
    rrep_sign(&r);

    /* Verifier is back on the real (here: unprovisioned fallback) key. */
    network_key_clear();
    TEST_ASSERT_FALSE(rrep_verify(&r));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rrep_sign_verify_round_trip);
    RUN_TEST(test_rrep_seq_covered_by_mac);
    RUN_TEST(test_rrep_verify_rejects_flipped_route_metric);
    RUN_TEST(test_rrep_verify_survives_relay_rewrite);
    RUN_TEST(test_rrep_verify_rejects_wrong_key_forgery);
    return UNITY_END();
}
