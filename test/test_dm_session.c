#include "unity.h"
#include "dm_session.h"
#include "../components/crypto/crypto_host.c"
#include "../components/dm_session/dm_session.c"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_quad_dh_both_sides_agree(void) {
    bramble_identity_t a_id, b_id, a_eph, b_eph;
    crypto_generate_identity(&a_id);
    crypto_generate_identity(&b_id);
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);
    uint32_t lo = a_id.address < b_id.address ? a_id.address : b_id.address;
    uint32_t hi = a_id.address < b_id.address ? b_id.address : a_id.address;

    uint8_t ka[32], kb[32];
    TEST_ASSERT_EQUAL(0, dm_derive_session_key(a_id.private_key, a_eph.private_key, b_id.public_key,
                                               b_eph.public_key, lo, hi, 0, ka));
    TEST_ASSERT_EQUAL(0, dm_derive_session_key(b_id.private_key, b_eph.private_key, a_id.public_key,
                                               a_eph.public_key, lo, hi, 0, kb));
    TEST_ASSERT_EQUAL_MEMORY(ka, kb, 32);
}

void test_different_epoch_yields_different_key(void) {
    bramble_identity_t a_id, b_id, a_eph, b_eph;
    crypto_generate_identity(&a_id);
    crypto_generate_identity(&b_id);
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);
    uint8_t k0[32], k1[32];
    dm_derive_session_key(a_id.private_key, a_eph.private_key, b_id.public_key, b_eph.public_key,
                          a_id.address, b_id.address, 0, k0);
    dm_derive_session_key(a_id.private_key, a_eph.private_key, b_id.public_key, b_eph.public_key,
                          a_id.address, b_id.address, 1, k1);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(k0, k1, 32));
}

/* RFC 7748 low-order points: for ANY scalar these force an all-zero shared
 * secret. All-zero (u=0) is the trivial case OpenSSL already rejects upstream,
 * so it proves nothing about our added guard. Use a NON-trivial low-order
 * u-coordinate (order 8, little-endian) so the vector is real; the accumulator
 * guard is what protects the mbedtls DEVICE path, which does not reject these. */
static const uint8_t k_low_order_u[32] = {
    0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
    0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00};
void test_low_order_dh_rejected(void) {
    uint8_t priv[32];
    crypto_random(priv, 32);
    uint8_t out[32];
    /* Contract: crypto_x25519_dh must fail on a low-order peer point. On host
     * OpenSSL rejects it upstream; on device only the added accumulator guard
     * catches it. This asserts the CONTRACT, not the red phase (see note below). */
    TEST_ASSERT_NOT_EQUAL(0, crypto_x25519_dh(priv, k_low_order_u, out));
}
/* Red-phase / device-path proof: the added guard is `acc |= ss[i]; if(!acc)
 * return -1;`. Because it is inline in crypto_x25519_dh and the host primitive
 * short-circuits first, this host test cannot exercise the guard directly.
 * Prove it against the real target with a shared-secret-level assertion: */
void test_zero_shared_secret_guard(void) {
    /* Simulate what the mbedtls path hands back for a low-order point: an
     * all-zero shared secret. crypto_x25519_check_shared (extracted guard,
     * added below) must reject it. */
    uint8_t zero_ss[32] = {0};
    uint8_t ok_ss[32];
    crypto_random(ok_ss, 32);
    ok_ss[0] |= 1;
    TEST_ASSERT_NOT_EQUAL(0, crypto_x25519_check_shared(zero_ss));
    TEST_ASSERT_EQUAL(0, crypto_x25519_check_shared(ok_ss));
}

/* Fix 1: pin ct_le32's correctness (not just its timing). ct_le32 is a
 * static helper in dm_session.c, visible here via the direct source
 * #include above (host-test convention). Big-endian: a[0] is most
 * significant, so a difference there must dominate a later byte even if
 * every later byte of the "smaller" array is larger. */
void test_ct_le32_matches_lexicographic_order(void) {
    uint8_t a[32], b[32];

    /* Differ only in the last (least significant) byte. */
    memset(a, 0x00, sizeof(a));
    a[31] = 0x01;
    memset(b, 0x00, sizeof(b));
    b[31] = 0x02;
    TEST_ASSERT_EQUAL(1, ct_le32(a, b));
    TEST_ASSERT_EQUAL(0, ct_le32(b, a));

    /* Differ only in the first (most significant) byte, with every other
     * byte pointing the "wrong" way: a's tail is all 0xFF, b's tail is all
     * 0x00. The first-byte difference must still decide it. */
    memset(a, 0xFF, sizeof(a));
    a[0] = 0x01;
    memset(b, 0x00, sizeof(b));
    b[0] = 0x02;
    TEST_ASSERT_EQUAL(1, ct_le32(a, b));
    TEST_ASSERT_EQUAL(0, ct_le32(b, a));

    /* Equal arrays: a tie counts as <=. */
    memset(a, 0x42, sizeof(a));
    memset(b, 0x42, sizeof(b));
    TEST_ASSERT_EQUAL(1, ct_le32(a, b));
    TEST_ASSERT_EQUAL(1, ct_le32(b, a));
}

/* Fix 1: SAS known-vector pin. Expected value computed once offline with
 * the exact HKDF-SHA256(salt="bramble-sas", ikm, info=NULL) call
 * dm_derive_sas makes, over ikm[i] = i for i in 0..127. */
void test_sas_known_vector(void) {
    uint8_t ikm[128];
    for (int i = 0; i < 128; i++)
        ikm[i] = (uint8_t)i;

    char sas[8];
    TEST_ASSERT_EQUAL(0, dm_derive_sas(ikm, sas));
    TEST_ASSERT_EQUAL_STRING("2805852", sas);
    TEST_ASSERT_EQUAL(7, strlen(sas));
}

/* Fix 1: both parties, deriving the SAS from their own independently
 * computed (but equal, per test_quad_dh_both_sides_agree) 128-byte IKM,
 * must land on the same human-comparable string. dm_compute_ikm is a
 * static helper in dm_session.c (visible via the direct #include), used
 * here only to capture the raw IKM each side derives; dm_derive_session_key
 * itself is unchanged. */
void test_sas_both_sides_agree(void) {
    bramble_identity_t a_id, b_id, a_eph, b_eph;
    crypto_generate_identity(&a_id);
    crypto_generate_identity(&b_id);
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);

    uint8_t ikm_a[128], ikm_b[128];
    TEST_ASSERT_EQUAL(0, dm_compute_ikm(a_id.private_key, a_eph.private_key, b_id.public_key,
                                        b_eph.public_key, ikm_a));
    TEST_ASSERT_EQUAL(0, dm_compute_ikm(b_id.private_key, b_eph.private_key, a_id.public_key,
                                        a_eph.public_key, ikm_b));
    TEST_ASSERT_EQUAL_MEMORY(ikm_a, ikm_b, 128); /* same underlying fix as 0.1.1's key agreement */

    char sas_a[8], sas_b[8];
    TEST_ASSERT_EQUAL(0, dm_derive_sas(ikm_a, sas_a));
    TEST_ASSERT_EQUAL(0, dm_derive_sas(ikm_b, sas_b));
    TEST_ASSERT_EQUAL_STRING(sas_a, sas_b);
}

/* Task 1.2: session table, state machine, eviction. */
void test_lookup_after_alloc(void) {
    dm_table_t t;
    dm_table_init(&t);
    dm_session_t* s = dm_alloc(&t, 0xAA, 0);
    TEST_ASSERT_NOT_NULL(s);
    s->state = DM_STATE_ACTIVE;
    TEST_ASSERT_EQUAL_PTR(s, dm_lookup(&t, 0xAA));
}
void test_handshaking_cap_enforced(void) {
    dm_table_t t;
    dm_table_init(&t);
    int allocated = 0;
    for (uint32_t a = 1; a <= DM_MAX_SESSIONS + 2; a++) {
        dm_session_t* s = dm_alloc(&t, a, 0);
        if (s) {
            s->state = DM_STATE_HANDSHAKING;
            allocated++;
        }
    }
    TEST_ASSERT_EQUAL(DM_MAX_HANDSHAKING, allocated);
}
void test_verified_active_not_evicted_for_handshaking(void) {
    dm_table_t t;
    dm_table_init(&t);
    dm_session_t* act = dm_alloc(&t, 0xAA, 0);
    act->state = DM_STATE_ACTIVE;
    act->verified = 1;
    for (uint32_t a = 1; a <= DM_MAX_HANDSHAKING; a++) {
        dm_session_t* s = dm_alloc(&t, 0x1000 + a, 1);
        if (s)
            s->state = DM_STATE_HANDSHAKING;
    }
    TEST_ASSERT_EQUAL_PTR(act, dm_lookup(&t, 0xAA)); /* survived: VERIFIED-ACTIVE protected */
}

/* Additional case beyond the brief: the three tests above never actually
 * exhaust DM_MAX_SESSIONS free slots (32 total vs. at most 1 + 8 used), so
 * none of them force the eviction branch itself to run; they only prove the
 * handshaking cap and that unrelated allocations don't clobber a VERIFIED
 * ACTIVE slot by accident. Fill the table completely (VERIFIED ACTIVE
 * sessions established FIRST with the SMALLEST timestamps, so they would be
 * the "oldest" and thus the natural LRU-eviction targets if ACTIVE
 * protection were broken, then two evictable HANDSHAKING slots established
 * LATER with larger timestamps) so the free-slot search must fail and the
 * real LRU-eviction path must run. A correct implementation must skip every
 * VERIFIED ACTIVE slot despite it being numerically older, and still evict
 * the oldest HANDSHAKING slot: only the protection check, not timestamp
 * order, explains that outcome, so this discriminates a broken/removed
 * VERIFIED-ACTIVE guard from a correct one (unlike naive timestamp choices
 * where the correct victim happens to also be the global minimum either
 * way). */
void test_lru_eviction_prefers_oldest_handshaking_over_verified_active(void) {
    dm_table_t t;
    dm_table_init(&t);

    for (uint32_t a = 1; a <= DM_MAX_SESSIONS - 2; a++) {
        dm_session_t* s = dm_alloc(&t, a, a); /* established_ms = a: 1..30, smaller than below */
        TEST_ASSERT_NOT_NULL(s);
        s->state = DM_STATE_ACTIVE;
        s->verified = 1; /* VERIFIED ACTIVE: must never be an eviction victim */
    }

    dm_session_t* older_hs = dm_alloc(&t, 0x9999, 1000);
    TEST_ASSERT_NOT_NULL(older_hs);
    older_hs->state = DM_STATE_HANDSHAKING;

    dm_session_t* newer_hs = dm_alloc(&t, 0x8888, 1050);
    TEST_ASSERT_NOT_NULL(newer_hs);
    newer_hs->state = DM_STATE_HANDSHAKING;
    /* Table is now completely full: 30 VERIFIED ACTIVE + 2 HANDSHAKING = 32
     * slots, handshaking cap nowhere near reached (2 of 8). Every ACTIVE
     * slot's timestamp (1..30) is smaller than either HANDSHAKING slot's
     * (1000, 1050). */

    dm_session_t* evicted_in = dm_alloc(&t, 0xBEEF, 5000);
    TEST_ASSERT_NOT_NULL(evicted_in);            /* must succeed via eviction, not NULL */
    TEST_ASSERT_EQUAL_PTR(older_hs, evicted_in); /* oldest EVICTABLE, not a VERIFIED ACTIVE slot */
    TEST_ASSERT_EQUAL_UINT32(0xBEEF, evicted_in->peer_addr);

    TEST_ASSERT_EQUAL_PTR(newer_hs, dm_lookup(&t, 0x8888)); /* untouched */
    for (uint32_t a = 1; a <= DM_MAX_SESSIONS - 2; a++) {
        dm_session_t* s = dm_lookup(&t, a);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_EQUAL(DM_STATE_ACTIVE, s->state); /* every VERIFIED ACTIVE slot survived */
    }
}

/* Fix 1 (red-team panel, cross-task finding): process_ke_init in
 * main/mesh_task.c allocates a first-contact INIT straight to
 * DM_STATE_ACTIVE with verified=0 (SAS confirmation is a separate UX step,
 * not gating establishment), never touching DM_STATE_HANDSHAKING or its
 * DM_MAX_HANDSHAKING cap. A first-contact INIT (no pin for the claimed
 * address) verifies for ANY self-chosen keypair (no secret required, no
 * real victim address needed, and post-Phase-4 the address is not bound to
 * the X25519 key at all), so an attacker can mint DM_MAX_SESSIONS forged first-contact
 * INITs from freshly-generated identities. Before this fix, dm_alloc's
 * eviction excluded every DM_STATE_ACTIVE slot regardless of `verified`, so
 * those forged slots were permanently unevictable: all future DM
 * establishment (with anyone, not just the attacker) died until reboot.
 * This test proves the fix: filling the table entirely with UNVERIFIED
 * ACTIVE slots (simulating the attack) must NOT prevent a subsequent
 * legitimate allocation. */
void test_unverified_active_evictable_under_pressure(void) {
    dm_table_t t;
    dm_table_init(&t);
    for (uint32_t a = 1; a <= DM_MAX_SESSIONS; a++) {
        dm_session_t* s = dm_alloc(&t, a, a); /* last_active_ms = a, ascending */
        TEST_ASSERT_NOT_NULL(s);
        s->state = DM_STATE_ACTIVE;
        s->verified = 0; /* forged first-contact: never confirmed */
    }
    /* Table is completely full (32/32 UNVERIFIED ACTIVE), zero free slots,
     * handshaking cap nowhere near reached (0 of 8): a fix that only
     * touched the handshaking cap would still return NULL here. The real
     * fix must make an UNVERIFIED ACTIVE slot itself evictable. */
    dm_session_t* legit = dm_alloc(&t, 0xC0FFEE, 1000);
    TEST_ASSERT_NOT_NULL(legit); /* must succeed via eviction, not permanently NULL */
    TEST_ASSERT_EQUAL_UINT32(0xC0FFEE, legit->peer_addr);
    TEST_ASSERT_NULL(dm_lookup(&t, 1)); /* the OLDEST (by last_active_ms) was evicted */
    for (uint32_t a = 2; a <= DM_MAX_SESSIONS; a++) {
        TEST_ASSERT_NOT_NULL(dm_lookup(&t, a)); /* every other forged slot untouched */
    }
}

/* A genuinely-active UNVERIFIED session (a real first-contact conversation
 * before the user runs the SAS check) must not be the eviction victim just
 * because it was established first: activity, not establishment order,
 * must drive LRU choice, or a real conversation could be evicted out from
 * under its own user by a flood of newer-but-idle forged sessions. */
void test_recently_active_unverified_session_survives_eviction(void) {
    dm_table_t t;
    dm_table_init(&t);

    dm_session_t* real = dm_alloc(&t, 0xAAAA, 1); /* oldest by establishment */
    TEST_ASSERT_NOT_NULL(real);
    real->state = DM_STATE_ACTIVE;
    real->verified = 0;

    for (uint32_t a = 1; a < DM_MAX_SESSIONS; a++) {
        dm_session_t* s = dm_alloc(&t, 0x1000 + a, 10 + a); /* established later */
        TEST_ASSERT_NOT_NULL(s);
        s->state = DM_STATE_ACTIVE;
        s->verified = 0;
    }
    /* Table full: 1 (real, oldest) + 31 (attacker filler) = 32. */

    real->last_active_ms = 100000; /* real's user sends/receives: activity bump */

    dm_session_t* legit = dm_alloc(&t, 0xC0FFEE, 999999);
    TEST_ASSERT_NOT_NULL(legit);                 /* still succeeds via eviction of a filler slot */
    TEST_ASSERT_NOT_NULL(dm_lookup(&t, 0xAAAA)); /* real survives: recently active */
}

/* Task 1.3: handshake INIT/RESP build/parse and authentication tags.
 *
 * NOTE on dm_build_init's signature: the brief lists it without a
 * my_eph_priv parameter, but the rekey-path tag's DH3 term is
 * X25519(my_eph_priv, peer_id_pub) (RFC section 1's B1 construction), which
 * is impossible to compute without the initiator's own ephemeral private
 * key. dm_build_resp's own brief signature already carries both
 * my_eph_pub and my_eph_priv for the identical reason (it needs eph_priv
 * for the quad-DH). Added my_eph_priv to dm_build_init to match; see the
 * task report for the full trace. */
void test_rekey_init_tag_verifies_and_fails_on_flip(void) {
    bramble_identity_t a, b;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    bramble_identity_t a_eph;
    crypto_generate_identity(&a_eph);

    bramble_key_exchange_t init;
    TEST_ASSERT_EQUAL(0, dm_build_init(&a, a_eph.public_key, a_eph.private_key, b.address, 0,
                                       b.public_key, &init));
    /* Non-zero tag: this is the rekey path, not first contact. */
    uint8_t zero16[16] = {0};
    TEST_ASSERT_NOT_EQUAL(0, memcmp(zero16, init.auth_tag, 16));
    TEST_ASSERT_EQUAL(0, dm_verify_init(&init, &b, 1, a.public_key, NULL));

    /* Flip a single tag byte: must be rejected. */
    bramble_key_exchange_t tampered = init;
    tampered.auth_tag[0] ^= 0x01;
    TEST_ASSERT_NOT_EQUAL(0, dm_verify_init(&tampered, &b, 1, a.public_key, NULL));
}

/* Fix 1: explicit zero-tag-downgrade case, distinct from the flipped-tag
 * case above. An attacker who intercepts a rekey INIT and zeroes its tag
 * (rather than flipping a byte) is trying to downgrade a rekey exchange
 * into looking like an unauthenticated first-contact one; the verifier
 * must still reject it when it believes have_peer_id (it knows this is
 * supposed to be a rekey, so an all-zero tag is never legitimate here). */
void test_rekey_init_zero_tag_rejected(void) {
    bramble_identity_t a, b;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    bramble_identity_t a_eph;
    crypto_generate_identity(&a_eph);

    bramble_key_exchange_t init;
    TEST_ASSERT_EQUAL(0, dm_build_init(&a, a_eph.public_key, a_eph.private_key, b.address, 0,
                                       b.public_key, &init));

    bramble_key_exchange_t downgraded = init;
    memset(downgraded.auth_tag, 0, sizeof(downgraded.auth_tag));
    TEST_ASSERT_NOT_EQUAL(0, dm_verify_init(&downgraded, &b, 1, a.public_key, NULL));
}

void test_first_contact_init_zero_tag_accepted_without_pin(void) {
    bramble_identity_t a, b;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    bramble_identity_t a_eph;
    crypto_generate_identity(&a_eph);

    bramble_key_exchange_t init;
    TEST_ASSERT_EQUAL(
        0, dm_build_init(&a, a_eph.public_key, a_eph.private_key, b.address, 0, NULL, &init));

    uint8_t zero16[16] = {0};
    TEST_ASSERT_EQUAL_MEMORY(zero16, init.auth_tag, 16);
    /* No pin for the peer: first contact proceeds TOFU-grade (the Phase 4
     * rebind removed the derive_address(long_term_pubkey) binding; the
     * address now derives from the Ed25519 identity key, which this X25519
     * handshake message does not carry). The stated residual: this window
     * closes only once the peer's attestation is heard and pinned. */
    TEST_ASSERT_EQUAL(0, dm_verify_init(&init, &b, 0, NULL, NULL));
}

/* Phase 4 DM key continuity: when the responder holds an
 * attestation-verified pin for the initiator's address, the handshake's
 * long-term X25519 key must BE the pinned one. Control first (matching pin
 * accepted, non-vacuous), then a mismatched pin is refused with the
 * distinct red-flag code even though the message is otherwise identical
 * and would verify with no pin. */
void test_verify_init_enforces_pinned_x25519_continuity(void) {
    bramble_identity_t a, b;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    bramble_identity_t a_eph;
    crypto_generate_identity(&a_eph);

    bramble_key_exchange_t init;
    TEST_ASSERT_EQUAL(
        0, dm_build_init(&a, a_eph.public_key, a_eph.private_key, b.address, 0, NULL, &init));

    /* Control: pin matches the handshake's long-term key -> accepted. */
    TEST_ASSERT_EQUAL(0, dm_verify_init(&init, &b, 0, NULL, a.public_key));

    /* Pinned key differs (peer's DM key "changed"): refused, distinct code. */
    uint8_t other_pin[32];
    memcpy(other_pin, a.public_key, 32);
    other_pin[0] ^= 0x01;
    TEST_ASSERT_EQUAL(DM_VERIFY_ERR_PIN_MISMATCH, dm_verify_init(&init, &b, 0, NULL, other_pin));
}

/* Fix 1: dispatch-confusion guard. The first-contact path (have_peer_id=0)
 * does ONLY the address check, no tag verification at all, so ke_type is
 * its SOLE defense: a genuine, validly-addressed RESP has exactly as
 * legitimate an address binding as an INIT would, so relabeling one as the
 * other and feeding it to dm_verify_init must be caught by the ke_type
 * assertion specifically, since nothing else in this code path would
 * catch it. */
void test_verify_init_rejects_resp_relabeled_as_init(void) {
    bramble_identity_t a, b;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    bramble_identity_t a_eph, b_eph;
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);

    bramble_key_exchange_t init;
    TEST_ASSERT_EQUAL(
        0, dm_build_init(&a, a_eph.public_key, a_eph.private_key, b.address, 0, NULL, &init));
    bramble_key_exchange_t resp;
    uint8_t kb[32];
    TEST_ASSERT_EQUAL(0,
                      dm_build_resp(&b, b_eph.public_key, b_eph.private_key, &init, 0, &resp, kb));

    /* Feed the RESP as-is (genuine ke_type == KE_TYPE_RESP, unmodified) to
     * the INIT verifier: address binding alone would accept it (a RESP's
     * address binding is exactly as valid as an INIT's), so only the
     * ke_type assertion can catch the mismatch. */
    TEST_ASSERT_NOT_EQUAL(0, dm_verify_init(&resp, &a, 0, NULL, NULL));
}

void test_init_resp_roundtrip_session_key(void) {
    bramble_identity_t a, b;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    bramble_identity_t a_eph, b_eph;
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);

    bramble_key_exchange_t init;
    TEST_ASSERT_EQUAL(
        0, dm_build_init(&a, a_eph.public_key, a_eph.private_key, b.address, 0, NULL, &init));
    TEST_ASSERT_EQUAL(0, dm_verify_init(&init, &b, 0, NULL, NULL));

    bramble_key_exchange_t resp;
    uint8_t kb[32];
    TEST_ASSERT_EQUAL(0,
                      dm_build_resp(&b, b_eph.public_key, b_eph.private_key, &init, 0, &resp, kb));

    uint8_t ka[32];
    TEST_ASSERT_EQUAL(0,
                      dm_verify_resp(&resp, &a, a_eph.private_key, a_eph.public_key, 0, NULL, ka));
    TEST_ASSERT_EQUAL_MEMORY(ka, kb, 32);
}

void test_verify_resp_rejects_tampered_tag(void) {
    bramble_identity_t a, b;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    bramble_identity_t a_eph, b_eph;
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);

    bramble_key_exchange_t init;
    TEST_ASSERT_EQUAL(
        0, dm_build_init(&a, a_eph.public_key, a_eph.private_key, b.address, 0, NULL, &init));

    bramble_key_exchange_t resp;
    uint8_t kb[32];
    TEST_ASSERT_EQUAL(0,
                      dm_build_resp(&b, b_eph.public_key, b_eph.private_key, &init, 0, &resp, kb));

    resp.auth_tag[0] ^= 0x01; /* flip a single byte of the confirm tag */
    uint8_t ka[32];
    TEST_ASSERT_NOT_EQUAL(
        0, dm_verify_resp(&resp, &a, a_eph.private_key, a_eph.public_key, 0, NULL, ka));
}

/* Fix 1: dispatch-confusion guard for dm_verify_resp. ke_type is not part
 * of transcript_2 or the K_confirm HKDF label, so flipping ONLY this field
 * on an otherwise-genuine RESP leaves its tag still valid: without the
 * ke_type assertion, dm_verify_resp would accept a message that no longer
 * claims to be a RESP at all, purely because nothing else in the message
 * changed. This isolates the ke_type check specifically, unlike a tampered
 * address or tag (which the existing checks would also independently
 * catch). */
void test_verify_resp_rejects_wrong_ke_type(void) {
    bramble_identity_t a, b;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    bramble_identity_t a_eph, b_eph;
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);

    bramble_key_exchange_t init;
    TEST_ASSERT_EQUAL(
        0, dm_build_init(&a, a_eph.public_key, a_eph.private_key, b.address, 0, NULL, &init));
    bramble_key_exchange_t resp;
    uint8_t kb[32];
    TEST_ASSERT_EQUAL(0,
                      dm_build_resp(&b, b_eph.public_key, b_eph.private_key, &init, 0, &resp, kb));

    bramble_key_exchange_t confused = resp;
    confused.ke_type = KE_TYPE_INIT; /* only field changed; tag is still valid */
    uint8_t ka[32];
    TEST_ASSERT_NOT_EQUAL(
        0, dm_verify_resp(&confused, &a, a_eph.private_key, a_eph.public_key, 0, NULL, ka));
}

/* Spoofed-address mutation case, post-rebind edition: the pre-Phase-4
 * derive_address(long_term_pubkey) check is gone (the address derives from
 * the Ed25519 identity key now), so src_addr integrity on a RESP rests on
 * the K_confirm tag: transcript_2 binds both addresses, so a mutated
 * src_addr recomputes a different expected tag and the message is
 * rejected. This test pins that the tag really does carry that binding. */
void test_verify_resp_rejects_spoofed_address(void) {
    bramble_identity_t a, b;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    bramble_identity_t a_eph, b_eph;
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);

    bramble_key_exchange_t init;
    TEST_ASSERT_EQUAL(
        0, dm_build_init(&a, a_eph.public_key, a_eph.private_key, b.address, 0, NULL, &init));

    bramble_key_exchange_t resp;
    uint8_t kb[32];
    TEST_ASSERT_EQUAL(0,
                      dm_build_resp(&b, b_eph.public_key, b_eph.private_key, &init, 0, &resp, kb));

    resp.src_addr ^= 0x1; /* transcript_2 no longer matches the tag */
    uint8_t ka[32];
    TEST_ASSERT_NOT_EQUAL(
        0, dm_verify_resp(&resp, &a, a_eph.private_key, a_eph.public_key, 0, NULL, ka));
}

/* Phase 4 DM key continuity on the initiator side, same shape as the INIT
 * test: matching pin accepted (control), mismatched pin refused with the
 * red-flag code. */
void test_verify_resp_enforces_pinned_x25519_continuity(void) {
    bramble_identity_t a, b;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    bramble_identity_t a_eph, b_eph;
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);

    bramble_key_exchange_t init;
    TEST_ASSERT_EQUAL(
        0, dm_build_init(&a, a_eph.public_key, a_eph.private_key, b.address, 0, NULL, &init));
    bramble_key_exchange_t resp;
    uint8_t kb[32];
    TEST_ASSERT_EQUAL(0,
                      dm_build_resp(&b, b_eph.public_key, b_eph.private_key, &init, 0, &resp, kb));

    uint8_t ka[32];
    TEST_ASSERT_EQUAL(
        0, dm_verify_resp(&resp, &a, a_eph.private_key, a_eph.public_key, 0, b.public_key, ka));
    TEST_ASSERT_EQUAL_MEMORY(ka, kb, 32);

    uint8_t other_pin[32];
    memcpy(other_pin, b.public_key, 32);
    other_pin[0] ^= 0x01;
    TEST_ASSERT_EQUAL(
        DM_VERIFY_ERR_PIN_MISMATCH,
        dm_verify_resp(&resp, &a, a_eph.private_key, a_eph.public_key, 0, other_pin, ka));
}

/*
 * M2 TOFU-session teardown (P3b). dm_session_teardown drops one peer's slot
 * so a stale first-contact session can be reclaimed the instant the peer's
 * real (endorsed) identity pins under a different key.
 */
void test_teardown_removes_active_session(void) {
    dm_table_t t;
    dm_table_init(&t);
    dm_session_t* s = dm_alloc(&t, 0xAA, 0);
    s->state = DM_STATE_ACTIVE;
    s->verified = 1;
    TEST_ASSERT_TRUE(dm_session_teardown(&t, 0xAA));
    /* Slot is gone: state reset and no longer findable. */
    TEST_ASSERT_NULL(dm_lookup(&t, 0xAA));
    TEST_ASSERT_EQUAL(DM_STATE_NONE, s->state);
}

void test_teardown_absent_addr_returns_false(void) {
    dm_table_t t;
    dm_table_init(&t);
    dm_session_t* s = dm_alloc(&t, 0xAA, 0);
    s->state = DM_STATE_ACTIVE;
    TEST_ASSERT_FALSE(dm_session_teardown(&t, 0xBB)); /* no slot for 0xBB */
    TEST_ASSERT_NOT_NULL(dm_lookup(&t, 0xAA));        /* the present one untouched */
}

void test_teardown_leaves_other_sessions(void) {
    dm_table_t t;
    dm_table_init(&t);
    dm_session_t* keep = dm_alloc(&t, 0x1111, 0);
    keep->state = DM_STATE_ACTIVE;
    keep->verified = 1;
    memset(keep->session_key, 0xAB, 32);
    dm_session_t* drop = dm_alloc(&t, 0x2222, 0);
    drop->state = DM_STATE_ACTIVE;

    TEST_ASSERT_TRUE(dm_session_teardown(&t, 0x2222));
    TEST_ASSERT_NULL(dm_lookup(&t, 0x2222));
    /* The sibling session is byte-for-byte intact. */
    dm_session_t* still = dm_lookup(&t, 0x1111);
    TEST_ASSERT_EQUAL_PTR(keep, still);
    TEST_ASSERT_EQUAL(DM_STATE_ACTIVE, still->state);
    for (int i = 0; i < 32; i++)
        TEST_ASSERT_EQUAL_HEX8(0xAB, still->session_key[i]);
}

/*
 * dm_pin_disagrees is the pure M2 decision the mesh_task hook applies under
 * s_dm_mutex: an ACTIVE session whose cached peer X25519 key differs from
 * the freshly-pinned (authenticated) binding is a stale TOFU session and
 * must be torn down. A matching key is the healthy case; a non-ACTIVE slot
 * is never a teardown target (dm_alloc's LRU reclaims handshaking slots).
 */
void test_pin_disagrees_differing_key_true(void) {
    dm_session_t s;
    memset(&s, 0, sizeof(s));
    s.state = DM_STATE_ACTIVE;
    memset(s.peer_id_pub, 0x11, 32);
    uint8_t pinned[32];
    memset(pinned, 0x22, 32); /* a different key than the session holds */
    TEST_ASSERT_TRUE(dm_pin_disagrees(&s, pinned));
}

void test_pin_disagrees_matching_key_false(void) {
    dm_session_t s;
    memset(&s, 0, sizeof(s));
    s.state = DM_STATE_ACTIVE;
    memset(s.peer_id_pub, 0x11, 32);
    uint8_t pinned[32];
    memset(pinned, 0x11, 32); /* identical: the healthy pinned session */
    TEST_ASSERT_FALSE(dm_pin_disagrees(&s, pinned));
}

void test_pin_disagrees_non_active_false(void) {
    dm_session_t s;
    memset(&s, 0, sizeof(s));
    memset(s.peer_id_pub, 0x11, 32);
    uint8_t pinned[32];
    memset(pinned, 0x22, 32); /* would differ, but slot is not ACTIVE */
    s.state = DM_STATE_HANDSHAKING;
    TEST_ASSERT_FALSE(dm_pin_disagrees(&s, pinned));
    s.state = DM_STATE_NONE;
    TEST_ASSERT_FALSE(dm_pin_disagrees(&s, pinned));
}

/*
 * dm_verified_should_clear (Task 7) is the pure decision the mesh_task pin-
 * mismatch handler applies alongside dm_pin_disagrees: only a VERIFIED
 * session whose pin genuinely disagrees needs its verified bit cleared.
 * Nothing to clear on an already-unverified session, and no clear when the
 * pin still matches (the healthy case).
 */
void test_verified_cleared_on_pin_disagreement(void) {
    dm_session_t s;
    memset(&s, 0, sizeof(s));
    s.state = DM_STATE_ACTIVE;
    s.verified = 1;
    memset(s.peer_id_pub, 0x11, 32);
    uint8_t new_pin[32];
    memset(new_pin, 0x22, 32); /* key changed */
    TEST_ASSERT_TRUE(dm_verified_should_clear(&s, new_pin));
    uint8_t same_pin[32];
    memset(same_pin, 0x11, 32);
    TEST_ASSERT_FALSE(dm_verified_should_clear(&s, same_pin)); /* same key: keep verified */
    s.verified = 0;
    TEST_ASSERT_FALSE(dm_verified_should_clear(&s, new_pin)); /* nothing to clear */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_quad_dh_both_sides_agree);
    RUN_TEST(test_different_epoch_yields_different_key);
    RUN_TEST(test_low_order_dh_rejected);
    RUN_TEST(test_zero_shared_secret_guard);
    RUN_TEST(test_ct_le32_matches_lexicographic_order);
    RUN_TEST(test_sas_known_vector);
    RUN_TEST(test_sas_both_sides_agree);
    RUN_TEST(test_lookup_after_alloc);
    RUN_TEST(test_handshaking_cap_enforced);
    RUN_TEST(test_verified_active_not_evicted_for_handshaking);
    RUN_TEST(test_lru_eviction_prefers_oldest_handshaking_over_verified_active);
    RUN_TEST(test_unverified_active_evictable_under_pressure);
    RUN_TEST(test_recently_active_unverified_session_survives_eviction);
    RUN_TEST(test_rekey_init_tag_verifies_and_fails_on_flip);
    RUN_TEST(test_rekey_init_zero_tag_rejected);
    RUN_TEST(test_first_contact_init_zero_tag_accepted_without_pin);
    RUN_TEST(test_verify_init_enforces_pinned_x25519_continuity);
    RUN_TEST(test_verify_init_rejects_resp_relabeled_as_init);
    RUN_TEST(test_init_resp_roundtrip_session_key);
    RUN_TEST(test_verify_resp_rejects_tampered_tag);
    RUN_TEST(test_verify_resp_rejects_wrong_ke_type);
    RUN_TEST(test_verify_resp_rejects_spoofed_address);
    RUN_TEST(test_verify_resp_enforces_pinned_x25519_continuity);
    RUN_TEST(test_teardown_removes_active_session);
    RUN_TEST(test_teardown_absent_addr_returns_false);
    RUN_TEST(test_teardown_leaves_other_sessions);
    RUN_TEST(test_pin_disagrees_differing_key_true);
    RUN_TEST(test_pin_disagrees_matching_key_false);
    RUN_TEST(test_pin_disagrees_non_active_false);
    RUN_TEST(test_verified_cleared_on_pin_disagreement);
    return UNITY_END();
}
