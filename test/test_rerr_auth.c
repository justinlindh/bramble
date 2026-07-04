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
 * Task 3.3 / SEC-H1 (STAGED, not closed: see network_key.h), extended by
 * ws 1.3b. rerr_sign/rerr_verify now authenticate reporter_addr||
 * broken_dest||broken_next_hop||seq, deliberately excluding only
 * header.packet_id (the one field every forwarder still rewrites on
 * re-origination that isn't otherwise covered). reporter_addr moved INTO
 * the MAC in ws 1.3b: mesh_task.c's send_rerr rebuilds a fresh struct with
 * its OWN reporter_addr and a freshly-drawn seq on every call (original
 * detection AND every re-origination), re-signing each time, so each hop
 * safely signs its own reporter_addr/seq pair. This is what makes keying
 * RERR replay on (reporter_addr, seq), both now authenticated, safe: each
 * reporter_addr is exactly one node drawing one monotonic counter, so the
 * per-key seq stream can't be interleaved by multiple signers. Confirmed
 * by reading send_rerr directly before writing any code here.
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

/* ws 1.3b: the seq is set by the caller (send_rerr draws it via
 * control_seq_next and writes it in before rerr_sign), mirroring the RREP
 * test file's set_seq helper. */
static void set_seq(bramble_rerr_t* r, uint64_t seq) {
    r->seq[0] = (uint8_t)(seq >> 40);
    r->seq[1] = (uint8_t)(seq >> 32);
    r->seq[2] = (uint8_t)(seq >> 24);
    r->seq[3] = (uint8_t)(seq >> 16);
    r->seq[4] = (uint8_t)(seq >> 8);
    r->seq[5] = (uint8_t)seq;
}

void test_rerr_sign_verify_round_trip(void) {
    bramble_rerr_t r = make_rerr(0x11111111, 0x1000);
    rerr_sign(&r);
    TEST_ASSERT_TRUE(rerr_verify(&r));
}

void test_rerr_verify_rejects_tampered_broken_dest(void) {
    bramble_rerr_t r = make_rerr(0x11111111, 0x1000);
    set_seq(&r, 0x0102030405);
    rerr_sign(&r);
    r.broken_dest ^= 0xFFFFFFFF; /* tamper an origin-stable field after signing */
    TEST_ASSERT_FALSE(rerr_verify(&r));
}

/*
 * ws 1.3b: reporter_addr is now MAC-covered (NEW property; it used to be
 * deliberately excluded). Safe because every re-origination re-signs with
 * the new hop's own reporter_addr (send_rerr's real behavior); nothing
 * ever needs the OLD hmac to survive a reporter_addr change anymore (see
 * test_rerr_verify_survives_reorigination below, which used to assert
 * exactly that and no longer does).
 */
void test_rerr_reporter_addr_covered(void) {
    bramble_rerr_t r = make_rerr(0x11111111, 0x1000);
    set_seq(&r, 0x0102030405);
    rerr_sign(&r);
    TEST_ASSERT_TRUE(rerr_verify(&r)); /* sanity: correctly signed */

    r.reporter_addr ^= 0xFFFFFFFF; /* tamper reporter_addr after signing */
    TEST_ASSERT_FALSE(rerr_verify(&r));
}

/* ws 1.3b: the 48-bit seq is part of the auth buffer, so tampering it
 * after signing must break verification exactly like tampering
 * broken_dest does above. */
void test_rerr_seq_covered(void) {
    bramble_rerr_t r = make_rerr(0x11111111, 0x1000);
    set_seq(&r, 0x0102030405);
    rerr_sign(&r);
    TEST_ASSERT_TRUE(rerr_verify(&r)); /* sanity: correctly signed with the seq included */

    r.seq[5] ^= 0xFF; /* tamper the seq after signing */
    TEST_ASSERT_FALSE(rerr_verify(&r));
}

/*
 * REWRITTEN for ws 1.3b (was the key regression case for the old
 * exclude-reporter_addr design: it asserted the ORIGINAL auth_hmac
 * survived unchanged onto a re-originated struct with a different
 * reporter_addr/packet_id). That assumption is now gone on purpose:
 * reporter_addr is MAC-covered, so a stale hmac from a different
 * reporter_addr must be REJECTED, not accepted. The real invariant is the
 * per-hop re-sign model: send_rerr rebuilds a fresh struct (its own
 * reporter_addr, a freshly-drawn seq, a fresh packet_id) and calls
 * rerr_sign again on every re-origination, so what must hold is "a fresh
 * sign over the new values verifies", not "the old signature survives".
 */
void test_rerr_verify_survives_reorigination(void) {
    bramble_rerr_t r = make_rerr(0x11111111, 0x1000);
    set_seq(&r, 0x0102030405);
    rerr_sign(&r);

    bramble_rerr_t reoriginated = r;
    reoriginated.reporter_addr = 0x22222222;
    reoriginated.header.packet_id = 0x2000;
    set_seq(&reoriginated, 0x0605040302);
    /* Sanity: the fields actually differ, or this test would pass
     * vacuously without exercising anything. */
    TEST_ASSERT_NOT_EQUAL(r.reporter_addr, reoriginated.reporter_addr);
    TEST_ASSERT_NOT_EQUAL(r.header.packet_id, reoriginated.header.packet_id);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(r.seq, reoriginated.seq, sizeof(r.seq)));

    /* The OLD assumption is gone: the untouched original hmac (still
     * carried over from r) must now reject against the re-originated
     * reporter_addr/seq. */
    TEST_ASSERT_FALSE(rerr_verify(&reoriginated));

    /* The per-hop re-sign model: re-originating and then re-signing (what
     * send_rerr actually does on every call) produces a fresh, valid
     * signature over the new reporter_addr/seq. */
    rerr_sign(&reoriginated);
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
    RUN_TEST(test_rerr_reporter_addr_covered);
    RUN_TEST(test_rerr_seq_covered);
    RUN_TEST(test_rerr_verify_survives_reorigination);
    RUN_TEST(test_rerr_verify_rejects_wrong_key_forgery);
    return UNITY_END();
}
