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
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000));
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
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000));
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
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000));

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
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 2000));
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
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000));
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
                      identity_store_handle_attestation(&s_store, &genuine, SELF_ADDR, 1000));

    /* Attacker: victim's address, attacker's keys, attacker's valid sig. */
    make_signed_attestation(&forged, victim_addr, ed_attacker, sk_attacker, 0x77);
    TEST_ASSERT_TRUE(crypto_derive_address(forged.ed25519_pub) != victim_addr);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_ADDR_MISMATCH,
                      identity_store_handle_attestation(&s_store, &forged, SELF_ADDR, 2000));

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
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000));

    /* Same keyholder (or an address-colliding key): rotated X25519,
     * re-signed, internally valid, addr check passes. */
    bramble_identity_attestation_t rotated = att;
    fill_key(rotated.x25519_pub, 0x99);
    resign_attestation(&rotated, sk);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_CONFLICT,
                      identity_store_handle_attestation(&s_store, &rotated, SELF_ADDR, 2000));
    TEST_ASSERT_EQUAL_UINT32(1, s_store.conflicts);

    const identity_pin_t* e = identity_store_lookup(&s_store, addr);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(att.x25519_pub, e->x25519_pub, 32); /* original kept */
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
    RUN_TEST(test_quorum_within_grace_unpinned_is_eligible);
    RUN_TEST(test_quorum_after_grace_unpinned_excluded_even_with_zero_pins);
    RUN_TEST(test_quorum_pinned_peer_eligible_within_and_after_grace);
    RUN_TEST(test_quorum_unestablished_never_eligible);
    return UNITY_END();
}
