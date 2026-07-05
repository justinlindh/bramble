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
 *   - not pinned            -> store (TOFU)
 *   - pinned, identical keys -> idempotent refresh (no churn; attestations
 *                              are replayable by design)
 *   - pinned, different keys -> CONFLICT: reject, keep original, count it
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

/* Build a fully signed attestation for `addr` under a freshly generated
 * Ed25519 keypair (returned in ed_pub/sk so tests can assert against it). */
static void make_signed_attestation(bramble_identity_attestation_t* p, uint32_t addr,
                                    uint8_t ed_pub[32], uint8_t sk[64], uint8_t x_seed) {
    memset(p, 0, sizeof(*p));
    p->header.version = BRAMBLE_VERSION;
    p->header.type = PKT_TYPE_IDENTITY_ATTESTATION;
    p->header.hop_limit = 8;
    p->header.dest_addr = 0xFFFFFFFFu;
    p->header.packet_id = 0x1000u + addr;
    p->src_addr = addr;
    fill_key(p->x25519_pub, x_seed);
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair(ed_pub, sk));
    memcpy(p->ed25519_pub, ed_pub, 32);
    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(p, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(sk, msg, sizeof(msg), p->sig));
}

/* ── identity_store_pin: raw TOFU semantics ─────────────────────────── */

static void test_first_pin_stores_and_lookup_returns_it(void) {
    identity_store_init(&s_store);
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
    identity_store_init(&s_store);
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
    identity_store_init(&s_store);
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
    identity_store_init(&s_store);
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
    identity_store_init(&s_store);
    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    make_signed_attestation(&att, 0xC0FFEEu, ed, sk, 0x40);

    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000));
    const identity_pin_t* e = identity_store_lookup(&s_store, 0xC0FFEEu);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ed, e->ed25519_pub, 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(att.x25519_pub, e->x25519_pub, 32);
}

static void test_bad_ed_sig_not_pinned_and_counted(void) {
    identity_store_init(&s_store);
    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    make_signed_attestation(&att, 0xC0FFEEu, ed, sk, 0x40);

    /* Non-vacuous: the untampered frame WOULD pin (checked in the test
     * above); here one covered byte flips and it must not. This is the
     * MAC-valid-but-sig-invalid case: a keyed member sent garbage. */
    att.x25519_pub[0] ^= 0x01;
    TEST_ASSERT_EQUAL(IDENTITY_PIN_BAD_SIG,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000));
    TEST_ASSERT_NULL(identity_store_lookup(&s_store, 0xC0FFEEu));
    TEST_ASSERT_EQUAL_UINT32(1, s_store.sig_failures);
}

static void test_self_attestation_ignored(void) {
    identity_store_init(&s_store);
    bramble_identity_attestation_t att;
    uint8_t ed[32], sk[64];
    make_signed_attestation(&att, SELF_ADDR, ed, sk, 0x40);

    TEST_ASSERT_EQUAL(IDENTITY_PIN_SELF,
                      identity_store_handle_attestation(&s_store, &att, SELF_ADDR, 1000));
    TEST_ASSERT_NULL(identity_store_lookup(&s_store, SELF_ADDR));
}

/* End-to-end impersonation through the delivery path: a keyed insider
 * (its frame reached delivery, i.e. its MAC was valid) attests the
 * victim's address under its OWN keypair with a VALID Ed25519 sig over
 * its own claim. The claim is internally consistent; it is the TOFU pin
 * that refuses the re-bind. */
static void test_impersonation_via_delivery_detected_and_refused(void) {
    identity_store_init(&s_store);

    bramble_identity_attestation_t genuine, forged;
    uint8_t ed_victim[32], sk_victim[64], ed_attacker[32], sk_attacker[64];
    make_signed_attestation(&genuine, 0xA11CEu, ed_victim, sk_victim, 0x40);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW,
                      identity_store_handle_attestation(&s_store, &genuine, SELF_ADDR, 1000));

    /* Attacker: victim's address, attacker's keys, attacker's valid sig. */
    make_signed_attestation(&forged, 0xA11CEu, ed_attacker, sk_attacker, 0x77);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_CONFLICT,
                      identity_store_handle_attestation(&s_store, &forged, SELF_ADDR, 2000));

    const identity_pin_t* e = identity_store_lookup(&s_store, 0xA11CEu);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ed_victim, e->ed25519_pub, 32);
    TEST_ASSERT_EQUAL_UINT32(1, s_store.conflicts);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_pin_stores_and_lookup_returns_it);
    RUN_TEST(test_identical_repin_is_idempotent_refresh);
    RUN_TEST(test_conflicting_keys_rejected_original_survives);
    RUN_TEST(test_lru_evicts_least_recently_confirmed);
    RUN_TEST(test_delivered_attestation_pins);
    RUN_TEST(test_bad_ed_sig_not_pinned_and_counted);
    RUN_TEST(test_self_attestation_ignored);
    RUN_TEST(test_impersonation_via_delivery_detected_and_refused);
    return UNITY_END();
}
