/*
 * Verified TOFU identity pin store (per-node identity Phase 3, Part C).
 *
 * The payoff of the whole identity campaign lives here: a keyed insider
 * attesting a victim's address with a DIFFERENT key is DETECTED and
 * REFUSED (first-seen wins; the original binding survives). The tests
 * below make that non-vacuous: the impersonation case first proves the
 * genuine binding pins and looks up correctly, then proves the
 * conflicting re-bind is rejected AND the lookup still returns the
 * ORIGINAL keys.
 *
 * Semantics pinned here:
 *   - src_addr != derive(ed25519_pub) -> ADDR MISMATCH: rejected even on
 *     first contact, counted (Phase 4 rebind: address impersonation is
 *     cryptographically infeasible, not merely TOFU-first-seen)
 *   - not pinned            -> store (TOFU)
 *   - pinned, identical keys -> idempotent refresh (no churn; attestations
 *                              are replayable by design)
 *   - pinned, different keys -> CONFLICT: reject, keep original, count it
 *     (reachable post-rebind via X25519 rotation under the same Ed key,
 *     or a 2^32-work address-colliding Ed key)
 *   - self address           -> ignored
 *   - MAC-valid, Ed-sig-invalid delivery -> not pinned, counted
 *   - bounded store (32), LRU eviction of least-recently-CONFIRMED
 *
 * RAM only this phase: pins reset on reboot and TOFU re-establishes (NVS
 * persistence is an explicit residual, not in scope).
 */
#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "packet.h"
#include "identity_store.h"

#include "../components/crypto/crypto_host.c"
#include "../components/packet/packet.c"
#include "../components/identity/identity_store.c"
/* identity_store.c now calls identity_endorsement_verify (trust-anchor P2),
 * defined in identity.c. identity.c is compiled as a SEPARATE source in
 * CMakeLists (not #included here) because its static put_be64/get_be64 helpers
 * would collide with packet.c's identically named statics in this TU. */
#include "identity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define SELF_ADDR 0x5E1F5E1Fu

static identity_store_t s_store;

static void fill_key(uint8_t key[32], uint8_t seed) {
    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)(seed + i);
}

/* Build a fully signed attestation under a freshly generated Ed25519
 * keypair (returned in ed_pub/sk so tests can assert against it).
 * claim_addr == 0 means the honest case: src_addr is the address the Ed
 * key actually derives to (Phase 4 rebind: that is the only src_addr a
 * receiver will accept). Non-zero claims that address instead, which is
 * exactly what an impersonating insider would have to send. Returns the
 * src_addr used. */
static uint32_t make_signed_attestation(bramble_identity_attestation_t* p, uint32_t claim_addr,
                                        uint8_t ed_pub[32], uint8_t sk[64], uint8_t x_seed) {
    memset(p, 0, sizeof(*p));
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair(ed_pub, sk));
    uint32_t addr = claim_addr != 0 ? claim_addr : crypto_derive_address(ed_pub);
    p->header.version = BRAMBLE_VERSION;
    p->header.type = PKT_TYPE_IDENTITY_ATTESTATION;
    p->header.hop_limit = 8;
    p->header.dest_addr = 0xFFFFFFFFu;
    p->header.packet_id = 0x1000u + addr;
    p->src_addr = addr;
    fill_key(p->x25519_pub, x_seed);
    memcpy(p->ed25519_pub, ed_pub, 32);
    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(p, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(sk, msg, sizeof(msg), p->sig));
    return addr;
}

/* Re-sign an attestation whose covered fields a test mutated (the mutation
 * itself stays internally valid: this models a KEYHOLDER sending a
 * different claim, not wire corruption). */
static void resign_attestation(bramble_identity_attestation_t* p, const uint8_t sk[64]) {
    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(p, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(sk, msg, sizeof(msg), p->sig));
}

/* ── identity_store_pin: raw TOFU semantics ─────────────────────────── */

static void test_first_pin_stores_and_lookup_returns_it(void) {
    identity_store_init(&s_store, 0);
    uint8_t ed[32], x[32];
    fill_key(ed, 0x10);
    fill_key(x, 0x50);

    TEST_ASSERT_NULL(identity_store_lookup(&s_store, 0xA1u));
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_pin(&s_store, 0xA1u, ed, x, 1000));

    const identity_pin_t* e = identity_store_lookup(&s_store, 0xA1u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX32(0xA1u, e->address);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ed, e->ed25519_pub, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(x, e->x25519_pub, 32);
    TEST_ASSERT_EQUAL_UINT32(1000, e->pinned_at_ms);
    TEST_ASSERT_EQUAL_UINT32(1000, e->last_confirmed_ms);
}

static void test_identical_repin_is_idempotent_refresh(void) {
    identity_store_init(&s_store, 0);
    uint8_t ed[32], x[32];
    fill_key(ed, 0x10);
    fill_key(x, 0x50);

    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_pin(&s_store, 0xA1u, ed, x, 1000));
    TEST_ASSERT_EQUAL(IDENTITY_PIN_REFRESHED, identity_store_pin(&s_store, 0xA1u, ed, x, 5000));

    const identity_pin_t* e = identity_store_lookup(&s_store, 0xA1u);
    TEST_ASSERT_NOT_NULL(e);
    /* Keys and pinned_at unchanged (no churn); last_confirmed refreshed
     * (that is what protects a live binding from LRU eviction). */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ed, e->ed25519_pub, 32);
    TEST_ASSERT_EQUAL_UINT32(1000, e->pinned_at_ms);
    TEST_ASSERT_EQUAL_UINT32(5000, e->last_confirmed_ms);
    TEST_ASSERT_EQUAL_UINT32(0, s_store.conflicts);
}

/* THE impersonation test: a conflicting binding for a pinned address is
 * rejected and the ORIGINAL binding survives, byte for byte. */
static void test_conflicting_keys_rejected_original_survives(void) {
    identity_store_init(&s_store, 0);
    uint8_t ed1[32], x1[32], ed2[32], x2[32];
    fill_key(ed1, 0x10);
    fill_key(x1, 0x50);
    fill_key(ed2, 0x90); /* the impersonator's different keys */
    fill_key(x2, 0xB0);

    /* Non-vacuous control: the genuine binding pins first. */
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_pin(&s_store, 0xA1u, ed1, x1, 1000));

    /* Impersonation attempt: same address, different keys. */
    TEST_ASSERT_EQUAL(IDENTITY_PIN_CONFLICT, identity_store_pin(&s_store, 0xA1u, ed2, x2, 2000));
    TEST_ASSERT_EQUAL_UINT32(1, s_store.conflicts);

    const identity_pin_t* e = identity_store_lookup(&s_store, 0xA1u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ed1, e->ed25519_pub, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(x1, e->x25519_pub, 32);
    /* A conflict is not a confirmation: it must not refresh the entry's
     * LRU position either (else an attacker could keep a victim's stale
     * binding pinned forever OR churn it, depending on sign). */
    TEST_ASSERT_EQUAL_UINT32(1000, e->last_confirmed_ms);

    /* X25519-only mismatch is also a conflict (the binding is the whole
     * key tuple, not just the Ed key). */
    TEST_ASSERT_EQUAL(IDENTITY_PIN_CONFLICT, identity_store_pin(&s_store, 0xA1u, ed1, x2, 3000));
    TEST_ASSERT_EQUAL_UINT32(2, s_store.conflicts);
}

static void test_lru_evicts_least_recently_confirmed(void) {
    identity_store_init(&s_store, 0);
    uint8_t ed[32], x[32];
    fill_key(x, 0x50);

    /* Fill to capacity: addresses 1..CAP pinned at t=100*i. */
    for (uint32_t i = 1; i <= IDENTITY_STORE_CAPACITY; i++) {
        fill_key(ed, (uint8_t)i);
        TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_pin(&s_store, i, ed, x, i * 100));
    }
    /* Re-confirm address 1 (the oldest) so address 2 becomes the LRU. */
    fill_key(ed, 1);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_REFRESHED, identity_store_pin(&s_store, 1u, ed, x, 999000));

    /* One past capacity: address 2 (least recently confirmed) is evicted;
     * 1 (just refreshed) and 3 (older pin, later confirm than 2? no,
     * 3's confirm is 300 > 2's 200) survive. */
    fill_key(ed, 0xEE);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_pin(&s_store, 0xFFu, ed, x, 999100));

    TEST_ASSERT_NULL(identity_store_lookup(&s_store, 2u));
    TEST_ASSERT_NOT_NULL(identity_store_lookup(&s_store, 1u));
    TEST_ASSERT_NOT_NULL(identity_store_lookup(&s_store, 3u));
    TEST_ASSERT_NOT_NULL(identity_store_lookup(&s_store, 0xFFu));
}

/* ── identity_store_handle_attestation: the delivery path ───────────── */

static void test_delivered_attestation_pins(void) {
    identity_store_init(&s_store, 0);
    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    uint32_t addr = make_signed_attestation(&att, 0, ed, sk, 0x40);

    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));
    const identity_pin_t* e = identity_store_lookup(&s_store, addr);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ed, e->ed25519_pub, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(att.x25519_pub, e->x25519_pub, 32);
}

static void test_bad_ed_sig_not_pinned_and_counted(void) {
    identity_store_init(&s_store, 0);
    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    uint32_t addr = make_signed_attestation(&att, 0, ed, sk, 0x40);

    /* Non-vacuous: the untampered frame WOULD pin (checked in the test
     * above); here one covered byte flips and it must not. This is the
     * MAC-valid-but-sig-invalid case: a keyed member sent garbage. */
    att.x25519_pub[0] ^= 0x01;
    TEST_ASSERT_EQUAL(IDENTITY_PIN_BAD_SIG,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));
    TEST_ASSERT_NULL(identity_store_lookup(&s_store, addr));
    TEST_ASSERT_EQUAL_UINT32(1, s_store.sig_failures);
}

/* THE PHASE 4 SECURITY PAYOFF: src_addr must BE the address the frame's
 * own Ed25519 key derives to. An attestation claiming any other address
 * is rejected EVEN ON FIRST CONTACT (no pin for that address exists yet),
 * upgrading address-impersonation resistance from TOFU-first-seen to
 * cryptographic: claiming a victim's address now requires a SHA256[0:4]
 * preimage under a key you hold. Non-vacuous: the control shows the same
 * keypair's HONEST claim pins fine through the identical code path. */
static void test_addr_mismatch_rejected_even_on_first_contact(void) {
    identity_store_init(&s_store, 0);
    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];

    /* Control: honest claim (src_addr == derive(ed_pub)) pins. */
    uint32_t honest = make_signed_attestation(&att, 0, ed, sk, 0x40);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));

    /* First-contact forgery: a fresh store, a victim address NOBODY has
     * pinned, an internally valid (validly signed) frame; only the
     * address<->key binding can reject it, and it must. */
    identity_store_init(&s_store, 0);
    uint32_t victim = honest ^ 0x1u;
    make_signed_attestation(&att, victim, ed, sk, 0x40);
    /* The fresh key's derived address is effectively random; assert the
     * mismatch explicitly so the test can never pass vacuously. */
    TEST_ASSERT_TRUE(crypto_derive_address(att.ed25519_pub) != victim);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_ADDR_MISMATCH,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 2000, 0));
    TEST_ASSERT_NULL(identity_store_lookup(&s_store, victim));
    TEST_ASSERT_EQUAL_UINT32(1, s_store.addr_mismatches);
    TEST_ASSERT_EQUAL_UINT32(0, s_store.conflicts);
}

static void test_self_attestation_ignored(void) {
    identity_store_init(&s_store, 0);
    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    /* Claiming OUR address (necessarily a mismatched claim for the
     * attacker's key): the self check fires first; our own store never
     * processes claims about ourselves at all. */
    make_signed_attestation(&att, SELF_ADDR, ed, sk, 0x40);

    TEST_ASSERT_EQUAL(IDENTITY_PIN_SELF,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));
    TEST_ASSERT_NULL(identity_store_lookup(&s_store, SELF_ADDR));
}

/* End-to-end impersonation through the delivery path, post-rebind: a
 * keyed insider (its frame reached delivery, i.e. its MAC was valid)
 * attests the victim's address under its OWN keypair with a VALID Ed25519
 * sig over its own claim. Pre-Phase-4 this was caught only if the victim
 * had been heard first (TOFU CONFLICT); now the address<->key binding
 * rejects it unconditionally, and the victim's pin is untouched. */
static void test_impersonation_via_delivery_detected_and_refused(void) {
    identity_store_init(&s_store, 0);

    bramble_identity_attestation_t genuine, forged;
    uint8_t ed_victim[32], sk_victim[64], ed_attacker[32], sk_attacker[64];
    uint32_t victim_addr = make_signed_attestation(&genuine, 0, ed_victim, sk_victim, 0x40);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW,
                      identity_store_handle_attestation(&s_store, &genuine, SELF_ADDR, 1000, 0));

    /* Attacker: victim's address, attacker's keys, attacker's valid sig. */
    make_signed_attestation(&forged, victim_addr, ed_attacker, sk_attacker, 0x77);
    TEST_ASSERT_TRUE(crypto_derive_address(forged.ed25519_pub) != victim_addr);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_ADDR_MISMATCH,
                      identity_store_handle_attestation(&s_store, &forged, SELF_ADDR, 2000, 0));

    const identity_pin_t* e = identity_store_lookup(&s_store, victim_addr);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ed_victim, e->ed25519_pub, 32);
    TEST_ASSERT_EQUAL_UINT32(1, s_store.addr_mismatches);
}

/* The conflict path still matters post-rebind: the address binds the Ed
 * key, but NOT the X25519 key. A validly signed re-attestation of the
 * same (address, Ed key) with a DIFFERENT X25519 key passes the addr
 * check and the sig check; only the TOFU pin can refuse it. This is the
 * DM-continuity red flag (a pinned peer's DM key must not silently
 * change), and it also covers the 2^32-work case of an attacker minting
 * an Ed key that collides with a victim's 4-byte address. */
static void test_x25519_rotation_is_conflict_via_delivery(void) {
    identity_store_init(&s_store, 0);

    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    uint32_t addr = make_signed_attestation(&att, 0, ed, sk, 0x40);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));

    /* Same keyholder (or an address-colliding key): rotated X25519,
     * re-signed, internally valid, addr check passes. */
    bramble_identity_attestation_t rotated = att;
    fill_key(rotated.x25519_pub, 0x99);
    resign_attestation(&rotated, sk);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_CONFLICT,
                      identity_store_handle_attestation(&s_store, &rotated, SELF_ADDR, 2000, 0));
    TEST_ASSERT_EQUAL_UINT32(1, s_store.conflicts);

    const identity_pin_t* e = identity_store_lookup(&s_store, addr);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(att.x25519_pub, e->x25519_pub, 32); /* original kept */
}

/* ── trust-anchor endorsement gate (P2) ────────────────────────────── */

/* Fixed anchor keypairs (deterministic, RFC 8032 seed expansion). ANCHOR is
 * the fleet anchor an anchored store is set to; OTHER stands in for a
 * different fleet's anchor to prove a wrong-anchor cert is refused. */
static const uint8_t ANCHOR_SEED[32] = {
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf};
static const uint8_t OTHER_ANCHOR_SEED[32] = {
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf};

/* Sign an endorsement of ed25519_pub with not_after under anchor_sk and write
 * the resulting cert (not_after + endorsement_sig) into att. Models the
 * offline anchor holder vouching for a node; the device never does this. */
static void endorse_into(bramble_identity_attestation_t* att, const uint8_t anchor_sk[64],
                         const uint8_t ed25519_pub[32], uint64_t not_after) {
    uint8_t emsg[IDENTITY_ENDORSEMENT_MSG_SIZE];
    TEST_ASSERT_EQUAL(IDENTITY_ENDORSEMENT_MSG_SIZE,
                      identity_endorsement_msg(ed25519_pub, not_after, emsg, sizeof(emsg)));
    att->not_after = not_after;
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(anchor_sk, emsg, sizeof(emsg), att->endorsement_sig));
}

/* THE opt-in guarantee: a store with NO anchor pins on the self-sig alone and
 * IGNORES the cert fields entirely, exactly as before P2. A frame with
 * not_after == 0 (no cert) still pins NEW. */
static void test_no_anchor_ignores_cert_and_pins(void) {
    identity_store_init(&s_store, 0);
    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    uint32_t addr = make_signed_attestation(&att, 0, ed, sk, 0x40);
    /* No cert on the wire (the default from make_signed_attestation's memset). */
    TEST_ASSERT_EQUAL_UINT64(IDENTITY_ENDORSEMENT_NOT_AFTER_NONE, att.not_after);
    TEST_ASSERT_FALSE(s_store.has_anchor);

    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));
    TEST_ASSERT_NOT_NULL(identity_store_lookup(&s_store, addr));
    TEST_ASSERT_EQUAL_UINT32(0, s_store.unendorsed);
    TEST_ASSERT_EQUAL_UINT32(0, s_store.expired);
}

/* Anchored + validly endorsed (permanent cert, real anchor sig): pins NEW. */
static void test_anchored_endorsed_pins(void) {
    identity_store_init(&s_store, 0);
    uint8_t anchor_pub[32], anchor_sk[64];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(ANCHOR_SEED, anchor_pub, anchor_sk));
    identity_store_set_anchor(&s_store, anchor_pub);
    TEST_ASSERT_TRUE(s_store.has_anchor);

    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    uint32_t addr = make_signed_attestation(&att, 0, ed, sk, 0x40);
    endorse_into(&att, anchor_sk, ed, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT);

    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));
    TEST_ASSERT_NOT_NULL(identity_store_lookup(&s_store, addr));
    TEST_ASSERT_EQUAL_UINT32(0, s_store.unendorsed);
}

/* Anchored + no cert (not_after == 0): UNENDORSED, not pinned, counted once. */
static void test_anchored_unendorsed_rejected(void) {
    identity_store_init(&s_store, 0);
    uint8_t anchor_pub[32], anchor_sk[64];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(ANCHOR_SEED, anchor_pub, anchor_sk));
    identity_store_set_anchor(&s_store, anchor_pub);

    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    uint32_t addr = make_signed_attestation(&att, 0, ed, sk, 0x40); /* no cert */

    TEST_ASSERT_EQUAL(IDENTITY_PIN_UNENDORSED,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));
    TEST_ASSERT_NULL(identity_store_lookup(&s_store, addr));
    TEST_ASSERT_EQUAL_UINT32(1, s_store.unendorsed);
}

/* Anchored + a cert that verifies against the WRONG anchor: UNENDORSED. The
 * cert is internally well-formed (a real signature by OTHER over this node's
 * key) but not by OUR anchor, so we refuse it. */
static void test_anchored_wrong_anchor_rejected(void) {
    identity_store_init(&s_store, 0);
    uint8_t anchor_pub[32], anchor_sk[64], other_pub[32], other_sk[64];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(ANCHOR_SEED, anchor_pub, anchor_sk));
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(OTHER_ANCHOR_SEED, other_pub, other_sk));
    identity_store_set_anchor(&s_store, anchor_pub);

    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    uint32_t addr = make_signed_attestation(&att, 0, ed, sk, 0x40);
    /* Signed by OTHER, not our anchor. */
    endorse_into(&att, other_sk, ed, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT);

    TEST_ASSERT_EQUAL(IDENTITY_PIN_UNENDORSED,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));
    TEST_ASSERT_NULL(identity_store_lookup(&s_store, addr));
    TEST_ASSERT_EQUAL_UINT32(1, s_store.unendorsed);
}

/* Anchored + a cert our anchor really signed, but for a DIFFERENT node's key
 * (cross-node graft): UNENDORSED. The endorsement message binds the exact
 * ed25519_pub, so a cert minted for node V cannot be lifted onto node A's
 * attestation. Non-vacuous: the SAME cert would verify for its true owner. */
static void test_anchored_cross_node_graft_rejected(void) {
    identity_store_init(&s_store, 0);
    uint8_t anchor_pub[32], anchor_sk[64];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(ANCHOR_SEED, anchor_pub, anchor_sk));
    identity_store_set_anchor(&s_store, anchor_pub);

    /* Victim node V gets a real cert from our anchor. */
    bramble_identity_attestation_t victim;
    uint8_t ed_v[32], sk_v[64];
    make_signed_attestation(&victim, 0, ed_v, sk_v, 0x22);
    uint8_t v_cert_sig[64];
    {
        uint8_t emsg[IDENTITY_ENDORSEMENT_MSG_SIZE];
        TEST_ASSERT_EQUAL(IDENTITY_ENDORSEMENT_MSG_SIZE,
                          identity_endorsement_msg(ed_v, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT,
                                                   emsg, sizeof(emsg)));
        TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(anchor_sk, emsg, sizeof(emsg), v_cert_sig));
    }

    /* Attacker A graft's V's cert onto its own (validly self-signed) frame. */
    bramble_identity_attestation_t att;
    uint8_t ed_a[32], sk_a[64];
    uint32_t addr_a = make_signed_attestation(&att, 0, ed_a, sk_a, 0x40);
    att.not_after = IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT;
    memcpy(att.endorsement_sig, v_cert_sig, 64);

    TEST_ASSERT_EQUAL(IDENTITY_PIN_UNENDORSED,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));
    TEST_ASSERT_NULL(identity_store_lookup(&s_store, addr_a));
    TEST_ASSERT_EQUAL_UINT32(1, s_store.unendorsed);

    /* Non-vacuous: V's cert DOES verify for V (same anchor, same key). */
    victim.not_after = IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT;
    memcpy(victim.endorsement_sig, v_cert_sig, 64);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW,
                      identity_store_handle_attestation(&s_store, &victim, SELF_ADDR, 1100, 0));
}

/* Expiry (v1 never issues a non-permanent cert, but the format is frozen now):
 *   - non-sentinel not_after in the past + a synced clock past it -> EXPIRED
 *   - the SAME cert with epoch_ms == 0 (unsynced) -> NOT enforced, pins
 *   - the permanent sentinel + any epoch -> never expires, pins */
static void test_anchored_expiry(void) {
    uint8_t anchor_pub[32], anchor_sk[64];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(ANCHOR_SEED, anchor_pub, anchor_sk));
    const uint64_t not_after = 1000000ull; /* ms epoch, arbitrary non-sentinel */

    /* Expired: clock is past not_after. */
    identity_store_init(&s_store, 0);
    identity_store_set_anchor(&s_store, anchor_pub);
    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    uint32_t addr = make_signed_attestation(&att, 0, ed, sk, 0x40);
    endorse_into(&att, anchor_sk, ed, not_after);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_EXPIRED, identity_store_handle_attestation(
                                                &s_store, &att, SELF_ADDR, 1000, not_after + 1));
    TEST_ASSERT_NULL(identity_store_lookup(&s_store, addr));
    TEST_ASSERT_EQUAL_UINT32(1, s_store.expired);

    /* Same cert, unsynced clock (epoch_ms == 0): expiry not enforced, pins. */
    identity_store_init(&s_store, 0);
    identity_store_set_anchor(&s_store, anchor_pub);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));
    TEST_ASSERT_NOT_NULL(identity_store_lookup(&s_store, addr));
    TEST_ASSERT_EQUAL_UINT32(0, s_store.expired);

    /* Permanent sentinel is never expired, even with a clock far in the future. */
    identity_store_init(&s_store, 0);
    identity_store_set_anchor(&s_store, anchor_pub);
    bramble_identity_attestation_t perm;
    uint8_t ed2[32], sk2[64];
    uint32_t addr2 = make_signed_attestation(&perm, 0, ed2, sk2, 0x60);
    endorse_into(&perm, anchor_sk, ed2, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_handle_attestation(
                                            &s_store, &perm, SELF_ADDR, 1000, 0xFFFFFFFFull));
    TEST_ASSERT_NOT_NULL(identity_store_lookup(&s_store, addr2));
    TEST_ASSERT_EQUAL_UINT32(0, s_store.expired);
}

/* Ordering: step 1 (addr<->key, self-sig) runs BEFORE the endorsement gate,
 * so a frame that is BOTH unendorsed AND addr-mismatched returns the existing
 * ADDR_MISMATCH code and increments addr_mismatches, not unendorsed. */
static void test_addr_mismatch_precedes_endorsement(void) {
    identity_store_init(&s_store, 0);
    uint8_t anchor_pub[32], anchor_sk[64];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(ANCHOR_SEED, anchor_pub, anchor_sk));
    identity_store_set_anchor(&s_store, anchor_pub);

    /* Claim a victim address the frame's own key does not derive to, and carry
     * no cert: both gates would reject, but addr-mismatch must win. */
    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    uint32_t victim = 0xDEADBEEFu;
    make_signed_attestation(&att, victim, ed, sk, 0x40);
    TEST_ASSERT_TRUE(crypto_derive_address(att.ed25519_pub) != victim);
    TEST_ASSERT_EQUAL_UINT64(IDENTITY_ENDORSEMENT_NOT_AFTER_NONE, att.not_after);

    TEST_ASSERT_EQUAL(IDENTITY_PIN_ADDR_MISMATCH,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));
    TEST_ASSERT_EQUAL_UINT32(1, s_store.addr_mismatches);
    TEST_ASSERT_EQUAL_UINT32(0, s_store.unendorsed);
}

/* ── identity_store_set_anchor: stale-pin drop on anchor change (P2 red-team) ─ */

/* THE runtime-hardening bug (P2 red-team): a fleet deployed un-anchored lets a
 * Sybil TOFU-pin normally; a later setAnchor to harden WITHOUT reboot must DROP
 * that un-endorsed pin, else it survives in entries[] and still feeds lookup /
 * quorum / DM continuity. First-ever anchoring clears the pin table. */
static void test_set_anchor_first_time_drops_tofu_pins(void) {
    identity_store_init(&s_store, 0);
    TEST_ASSERT_FALSE(s_store.has_anchor);

    /* An un-anchored node TOFU-pins a peer (the Sybil, pre-hardening). */
    uint8_t ed[32], x[32];
    fill_key(ed, 0x10);
    fill_key(x, 0x50);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_pin(&s_store, 0xA1u, ed, x, 1000));
    TEST_ASSERT_NOT_NULL(identity_store_lookup(&s_store, 0xA1u));

    /* Operator hardens at runtime: the stale un-endorsed pin must be dropped. */
    uint8_t anchor_pub[32], anchor_sk[64];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(ANCHOR_SEED, anchor_pub, anchor_sk));
    identity_store_set_anchor(&s_store, anchor_pub);
    TEST_ASSERT_TRUE(s_store.has_anchor);
    TEST_ASSERT_NULL(identity_store_lookup(&s_store, 0xA1u));
    TEST_ASSERT_EQUAL(0, identity_store_count(&s_store));
}

/* Idempotent re-provision of the SAME anchor must KEEP the endorsed pins (the
 * P1 idempotency concern): re-running setAnchor with the current key is a no-op
 * for the pin table. */
static void test_set_anchor_same_key_keeps_pins(void) {
    identity_store_init(&s_store, 0);
    uint8_t anchor_pub[32], anchor_sk[64];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(ANCHOR_SEED, anchor_pub, anchor_sk));
    identity_store_set_anchor(&s_store, anchor_pub);

    /* Pin an ENDORSED peer through the full gate. */
    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    uint32_t addr = make_signed_attestation(&att, 0, ed, sk, 0x40);
    endorse_into(&att, anchor_sk, ed, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));
    TEST_ASSERT_NOT_NULL(identity_store_lookup(&s_store, addr));

    /* Same key again: the endorsed pin SURVIVES. */
    identity_store_set_anchor(&s_store, anchor_pub);
    TEST_ASSERT_NOT_NULL(identity_store_lookup(&s_store, addr));
    TEST_ASSERT_EQUAL(1, identity_store_count(&s_store));
}

/* Rotation to a DIFFERENT anchor drops the pins: an endorsed pin under the OLD
 * anchor is dead under the new key, so it must not survive the rotation. */
static void test_set_anchor_rotation_drops_pins(void) {
    identity_store_init(&s_store, 0);
    uint8_t anchor_pub[32], anchor_sk[64], other_pub[32], other_sk[64];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(ANCHOR_SEED, anchor_pub, anchor_sk));
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair_from_seed(OTHER_ANCHOR_SEED, other_pub, other_sk));
    identity_store_set_anchor(&s_store, anchor_pub);

    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    uint32_t addr = make_signed_attestation(&att, 0, ed, sk, 0x40);
    endorse_into(&att, anchor_sk, ed, IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000, 0));
    TEST_ASSERT_NOT_NULL(identity_store_lookup(&s_store, addr));

    /* Rotate to a new anchor: the old endorsed pin is dropped, key updated. */
    identity_store_set_anchor(&s_store, other_pub);
    TEST_ASSERT_NULL(identity_store_lookup(&s_store, addr));
    TEST_ASSERT_EQUAL(0, identity_store_count(&s_store));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(other_pub, s_store.anchor_pub, 32);
}

/* ── identity_store_quorum_eligible: the Phase 4 timesync gate ──────── */

/* Semantics under test (documented on the function): established tenure
 * is ALWAYS required; a PINNED peer is always eligible (subject to
 * tenure); an UNPINNED peer is eligible ONLY within the bounded per-boot
 * grace (QUORUM_BOOTSTRAP_GRACE_MS from identity_store_init's now_ms), and
 * NEVER after it. That bounds the old unbounded "zero pins -> trust every
 * established peer" hole to a per-boot liveness window and closes the
 * NEW-SEC-4 bootstrap-quorum race. Boot reference here is 1000 ms. */
#define QBOOT 1000u

/* LIVENESS: a fresh mesh (zero pins) within the grace must still let an
 * established unpinned peer corroborate, or timesync could never bootstrap.
 * Non-vacuous: asserts TRUE at boot and just before the grace boundary. */
static void test_quorum_within_grace_unpinned_is_eligible(void) {
    identity_store_init(&s_store, QBOOT);
    TEST_ASSERT_EQUAL(0, identity_store_count(&s_store)); /* zero pins: fresh mesh */
    /* Right at boot. */
    TEST_ASSERT_TRUE(identity_store_quorum_eligible(&s_store, 0xA1u, true, QBOOT));
    /* One millisecond before the grace expires: still inside. */
    TEST_ASSERT_TRUE(identity_store_quorum_eligible(&s_store, 0xA1u, true,
                                                    QBOOT + QUORUM_BOOTSTRAP_GRACE_MS - 1u));
}

/* SECURITY (the race closed): once the grace has expired, an established
 * UNPINNED peer is NOT quorum-eligible even with ZERO pins held. An
 * unattested or Sybil node can no longer dominate the quorum and skew the
 * clock. Non-vacuous: asserts FALSE at the exact boundary and beyond. */
static void test_quorum_after_grace_unpinned_excluded_even_with_zero_pins(void) {
    identity_store_init(&s_store, QBOOT);
    TEST_ASSERT_EQUAL(0, identity_store_count(&s_store)); /* still zero pins */
    /* Exactly at the boundary (now - boot == GRACE): already excluded. */
    TEST_ASSERT_FALSE(
        identity_store_quorum_eligible(&s_store, 0xA1u, true, QBOOT + QUORUM_BOOTSTRAP_GRACE_MS));
    /* Well after: still excluded. */
    TEST_ASSERT_FALSE(identity_store_quorum_eligible(&s_store, 0xA1u, true,
                                                     QBOOT + QUORUM_BOOTSTRAP_GRACE_MS + 60000u));
}

/* A pinned peer corroborates both inside AND long after the grace: holding
 * a verified binding is what the gate ultimately wants. */
static void test_quorum_pinned_peer_eligible_within_and_after_grace(void) {
    identity_store_init(&s_store, QBOOT);
    uint8_t ed[32], x[32];
    fill_key(ed, 0x10);
    fill_key(x, 0x50);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_pin(&s_store, 0xA1u, ed, x, QBOOT));

    TEST_ASSERT_TRUE(identity_store_quorum_eligible(&s_store, 0xA1u, true, QBOOT));
    TEST_ASSERT_TRUE(identity_store_quorum_eligible(&s_store, 0xA1u, true,
                                                    QBOOT + QUORUM_BOOTSTRAP_GRACE_MS + 60000u));
    /* Meanwhile an established UNPINNED peer is already excluded once the
     * grace is over, even though a pin exists for a different address. */
    TEST_ASSERT_FALSE(identity_store_quorum_eligible(&s_store, 0xB2u, true,
                                                     QBOOT + QUORUM_BOOTSTRAP_GRACE_MS + 60000u));
}

/* Tenure is never relaxed: an UNestablished peer is ineligible regardless
 * of the grace or pin state (inside grace, after grace, pinned). */
static void test_quorum_unestablished_never_eligible(void) {
    identity_store_init(&s_store, QBOOT);
    /* Unpinned, inside grace, but not established. */
    TEST_ASSERT_FALSE(identity_store_quorum_eligible(&s_store, 0xA1u, false, QBOOT));
    /* Unpinned, after grace, not established. */
    TEST_ASSERT_FALSE(
        identity_store_quorum_eligible(&s_store, 0xA1u, false, QBOOT + QUORUM_BOOTSTRAP_GRACE_MS));
    /* Even pinned but not established stays out. */
    uint8_t ed[32], x[32];
    fill_key(ed, 0x10);
    fill_key(x, 0x50);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_pin(&s_store, 0xA1u, ed, x, QBOOT));
    TEST_ASSERT_FALSE(identity_store_quorum_eligible(&s_store, 0xA1u, false, QBOOT));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_pin_stores_and_lookup_returns_it);
    RUN_TEST(test_identical_repin_is_idempotent_refresh);
    RUN_TEST(test_conflicting_keys_rejected_original_survives);
    RUN_TEST(test_lru_evicts_least_recently_confirmed);
    RUN_TEST(test_delivered_attestation_pins);
    RUN_TEST(test_bad_ed_sig_not_pinned_and_counted);
    RUN_TEST(test_addr_mismatch_rejected_even_on_first_contact);
    RUN_TEST(test_self_attestation_ignored);
    RUN_TEST(test_impersonation_via_delivery_detected_and_refused);
    RUN_TEST(test_x25519_rotation_is_conflict_via_delivery);
    RUN_TEST(test_no_anchor_ignores_cert_and_pins);
    RUN_TEST(test_anchored_endorsed_pins);
    RUN_TEST(test_anchored_unendorsed_rejected);
    RUN_TEST(test_anchored_wrong_anchor_rejected);
    RUN_TEST(test_anchored_cross_node_graft_rejected);
    RUN_TEST(test_anchored_expiry);
    RUN_TEST(test_addr_mismatch_precedes_endorsement);
    RUN_TEST(test_set_anchor_first_time_drops_tofu_pins);
    RUN_TEST(test_set_anchor_same_key_keeps_pins);
    RUN_TEST(test_set_anchor_rotation_drops_pins);
    RUN_TEST(test_quorum_within_grace_unpinned_is_eligible);
    RUN_TEST(test_quorum_after_grace_unpinned_excluded_even_with_zero_pins);
    RUN_TEST(test_quorum_pinned_peer_eligible_within_and_after_grace);
    RUN_TEST(test_quorum_unestablished_never_eligible);
    return UNITY_END();
}
