#include "unity.h"
#include "dm_session.h"
#include "packet.h"
#include "../components/crypto/crypto_host.c"

/*
 * HKDF fault injection (issue #84 regression coverage). dm_session.c is
 * #included as source below, so redefining the symbol between the two includes
 * routes EVERY HKDF call dm_session.c makes through this shim while leaving the
 * real crypto_host.c implementation intact for everyone else. No test hook
 * exists in production code as a result.
 *
 * hkdf_fail_after(n): let the next n calls succeed, then fail every call after
 * that. hkdf_fail_off() restores normal behaviour. Armed per test.
 */
static int g_hkdf_allow = -1; /* <0 means "never fail" */
static int test_hkdf_sha256(const uint8_t* salt, size_t salt_len, const uint8_t* ikm,
                            size_t ikm_len, const uint8_t* info, size_t info_len, uint8_t* okm,
                            size_t okm_len) {
    if (g_hkdf_allow >= 0) {
        if (g_hkdf_allow == 0)
            return -1; /* the injected failure; okm deliberately left untouched */
        g_hkdf_allow--;
    }
    return crypto_hkdf_sha256(salt, salt_len, ikm, ikm_len, info, info_len, okm, okm_len);
}
static void hkdf_fail_after(int n) { g_hkdf_allow = n; }
static void hkdf_fail_off(void) { g_hkdf_allow = -1; }

#define crypto_hkdf_sha256 test_hkdf_sha256
#include "../components/dm_session/dm_session.c"
#undef crypto_hkdf_sha256
#include <string.h>

void setUp(void) { hkdf_fail_off(); }
void tearDown(void) { hkdf_fail_off(); }

static int all_zero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

/* Canonical addresses: lo=0x11111111, hi=0x22222222 (lo < hi already ordered). */
#define ADDR_LO 0x11111111u
#define ADDR_HI 0x22222222u

static void fill_ikm(uint8_t ikm[128]) {
    for (int i = 0; i < 128; i++)
        ikm[i] = (uint8_t)i;
}

/* RK_0 MUST equal today's session key for epoch 0 (migration continuity). */
void test_rk0_equals_legacy_session_key(void) {
    uint8_t ikm[128];
    fill_ikm(ikm);
    uint8_t rk[32], ck_lohi[32], ck_hilo[32];
    TEST_ASSERT_EQUAL(0, dm_ratchet_init(ikm, ADDR_LO, ADDR_HI, rk, ck_lohi, ck_hilo));
    uint8_t legacy[32];
    TEST_ASSERT_EQUAL(0, dm_session_key_from_ikm(ikm, ADDR_LO, ADDR_HI, 0, legacy));
    TEST_ASSERT_EQUAL_MEMORY(legacy, rk, 32);
}

void test_ratchet_init_kat(void) {
    uint8_t ikm[128];
    fill_ikm(ikm);
    uint8_t rk[32], ck_lohi[32], ck_hilo[32];
    TEST_ASSERT_EQUAL(0, dm_ratchet_init(ikm, ADDR_LO, ADDR_HI, rk, ck_lohi, ck_hilo));
    /* The two directional chains MUST differ (domain separation). */
    TEST_ASSERT_NOT_EQUAL(0, memcmp(ck_lohi, ck_hilo, 32));
    /* Committed vectors: HKDF(salt=RK_0, ikm="", info="bramble-dm-chain-<dir>")
     * over ikm[i]=i, lo=0x11111111, hi=0x22222222. Computed once, pinned here. */
    static const uint8_t k_ck_lohi[32] = {0x58, 0xc1, 0x5a, 0x2a, 0xff, 0x95, 0xf3, 0xe4,
                                          0x0f, 0x3f, 0x4d, 0x94, 0x0e, 0x07, 0x6d, 0x2f,
                                          0x55, 0x3d, 0x75, 0x4c, 0xa1, 0x3b, 0x6e, 0x72,
                                          0xfa, 0xc5, 0xc8, 0xa0, 0xba, 0xcd, 0x09, 0x64};
    static const uint8_t k_ck_hilo[32] = {0x72, 0xf7, 0x5c, 0xfd, 0x50, 0x0e, 0x48, 0xc3,
                                          0xbb, 0x70, 0xc8, 0xd1, 0x20, 0x67, 0xb4, 0x86,
                                          0x08, 0xbb, 0xea, 0x18, 0xbf, 0x91, 0x11, 0x17,
                                          0x1e, 0x0b, 0xc4, 0x9a, 0x0e, 0x2b, 0xa1, 0x27};
    TEST_ASSERT_EQUAL_MEMORY(k_ck_lohi, ck_lohi, 32);
    TEST_ASSERT_EQUAL_MEMORY(k_ck_hilo, ck_hilo, 32);
}

/* Message keys change every message; the chain advances; mk_n != mk_{n+1}. */
void test_chain_advances_and_mk_changes(void) {
    uint8_t ikm[128];
    fill_ikm(ikm);
    uint8_t rk[32], ck[32], ck_hilo[32];
    TEST_ASSERT_EQUAL(0, dm_ratchet_init(ikm, ADDR_LO, ADDR_HI, rk, ck, ck_hilo));
    uint8_t prev_mk[32] = {0};
    uint8_t seen_mk[8][32];
    for (uint16_t n = 0; n < 8; n++) {
        uint8_t mk[32], ck_next[32];
        dm_ratchet_step(ck, n, mk, ck_next);
        TEST_ASSERT_NOT_EQUAL(0, memcmp(mk, ck, 32));      /* mk != chain key */
        TEST_ASSERT_NOT_EQUAL(0, memcmp(ck_next, ck, 32)); /* chain advanced */
        if (n > 0)
            TEST_ASSERT_NOT_EQUAL(0, memcmp(mk, prev_mk, 32));
        memcpy(prev_mk, mk, 32);
        memcpy(seen_mk[n], mk, 32);
        memcpy(ck, ck_next, 32);
    }
    /* All eight message keys are pairwise distinct. */
    for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++)
            TEST_ASSERT_NOT_EQUAL(0, memcmp(seen_mk[i], seen_mk[j], 32));
}

/* MANDATORY (review gate 5): no (message key, nonce) pair repeats across a key
 * change. Simulate the sender: a monotonic counter nonce alongside a chain that
 * advances every message. Assert every (mk||nonce) pair is unique, so a GCM
 * key+nonce reuse (the catastrophic case) can never arise. */
void test_no_key_nonce_reuse(void) {
    uint8_t ikm[128];
    fill_ikm(ikm);
    uint8_t rk[32], ck[32], ck_hilo[32];
    TEST_ASSERT_EQUAL(0, dm_ratchet_init(ikm, ADDR_LO, ADDR_HI, rk, ck, ck_hilo));
    uint8_t pairs[16][44]; /* 32 key + 12 nonce */
    uint64_t nonce_ctr = 0xABCDEF00ull;
    for (uint16_t n = 0; n < 16; n++) {
        uint8_t mk[32], ck_next[32];
        dm_ratchet_step(ck, n, mk, ck_next);
        memcpy(pairs[n], mk, 32);
        for (int b = 0; b < 12; b++)
            pairs[n][32 + b] = (uint8_t)(nonce_ctr >> (8 * b));
        nonce_ctr++; /* monotonic, never reused */
        memcpy(ck, ck_next, 32);
    }
    for (int i = 0; i < 16; i++)
        for (int j = i + 1; j < 16; j++)
            TEST_ASSERT_NOT_EQUAL(0, memcmp(pairs[i], pairs[j], 44));
}

/* DH ratchet: a fresh DH advances the root; RK_{e+1} != RK_e and is a pure
 * function of (RK_e, dh, epoch). */
void test_dh_ratchet_advances_root(void) {
    uint8_t ikm[128];
    fill_ikm(ikm);
    uint8_t rk0[32], ck_lohi[32], ck_hilo[32];
    TEST_ASSERT_EQUAL(0, dm_ratchet_init(ikm, ADDR_LO, ADDR_HI, rk0, ck_lohi, ck_hilo));
    uint8_t dh[32];
    memset(dh, 0x5A, 32);
    uint8_t rk1a[32], rk1b[32];
    TEST_ASSERT_EQUAL(0, dm_ratchet_dh(rk0, dh, ADDR_LO, ADDR_HI, 1, rk1a));
    TEST_ASSERT_EQUAL(0, dm_ratchet_dh(rk0, dh, ADDR_LO, ADDR_HI, 1, rk1b));
    TEST_ASSERT_EQUAL_MEMORY(rk1a, rk1b, 32);        /* deterministic */
    TEST_ASSERT_NOT_EQUAL(0, memcmp(rk1a, rk0, 32)); /* advanced */
    /* Committed vector: HKDF(salt=RK_0, ikm=dh(0x5A*32), info=lo||hi||epoch=1). */
    static const uint8_t k_rk1[32] = {0x3f, 0x3e, 0xbd, 0xf2, 0x1a, 0x6a, 0x10, 0x13,
                                      0xbf, 0x33, 0x54, 0x87, 0xe2, 0xc4, 0x4c, 0x3d,
                                      0x79, 0x75, 0xf6, 0x3f, 0x6a, 0x92, 0xc2, 0x16,
                                      0xb4, 0x33, 0xa0, 0xae, 0xe6, 0xf4, 0x88, 0x22};
    TEST_ASSERT_EQUAL_MEMORY(k_rk1, rk1a, 32);
}

/* The framed ciphertext is exactly 3 bytes longer than the plaintext, and the
 * send index advances. */
void test_ratchet_encrypt_frames_header(void) {
    bramble_identity_t a, b, a_eph, b_eph;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);
    uint8_t ikm[128];
    TEST_ASSERT_EQUAL(
        0, dm_compute_ikm(a.private_key, a_eph.private_key, b.public_key, b_eph.public_key, ikm));
    dm_session_t s;
    memset(&s, 0, sizeof(s));
    s.peer_addr = b.address;
    s.state = DM_STATE_ACTIVE;
    dm_session_ratchet_init_state(&s, ikm, a.address, b.address);

    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION;
    h.type = PKT_TYPE_DATA;
    h.flags = FLAG_ENCRYPT;
    h.hop_limit = 8;
    h.dest_addr = b.address;
    h.packet_id = 0x1234;
    const uint8_t pt[] = "hi";
    uint8_t nonce[12];
    memset(nonce, 0x01, 12);
    uint8_t ct[DM_RATCHET_HEADER_SIZE + sizeof(pt)];
    uint8_t tag[16];
    size_t flen = 0;
    TEST_ASSERT_EQUAL(
        0, dm_session_ratchet_encrypt(&s, &h, a.address, pt, sizeof(pt), nonce, ct, tag, &flen));
    TEST_ASSERT_EQUAL(DM_RATCHET_HEADER_SIZE + sizeof(pt), flen);
    TEST_ASSERT_EQUAL_UINT16(1, s.ratchet.send.index); /* advanced 0 -> 1 */
}

/* Two encrypts use two distinct message keys (the chain advanced), so the same
 * plaintext + reused nonce yields a different tag: proves per-message keying. */
void test_ratchet_encrypt_advances_key(void) {
    bramble_identity_t a, b, a_eph, b_eph;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);
    uint8_t ikm[128];
    TEST_ASSERT_EQUAL(
        0, dm_compute_ikm(a.private_key, a_eph.private_key, b.public_key, b_eph.public_key, ikm));
    dm_session_t s;
    memset(&s, 0, sizeof(s));
    s.peer_addr = b.address;
    s.state = DM_STATE_ACTIVE;
    dm_session_ratchet_init_state(&s, ikm, a.address, b.address);
    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION;
    h.type = PKT_TYPE_DATA;
    h.flags = FLAG_ENCRYPT;
    h.hop_limit = 8;
    h.dest_addr = b.address;
    h.packet_id = 0x1234;
    const uint8_t pt[] = "same";
    uint8_t nonce[12];
    memset(nonce, 0x09, 12);
    uint8_t c0[DM_RATCHET_HEADER_SIZE + sizeof(pt)], c1[DM_RATCHET_HEADER_SIZE + sizeof(pt)];
    uint8_t t0[16], t1[16];
    size_t f0, f1;
    dm_session_ratchet_encrypt(&s, &h, a.address, pt, sizeof(pt), nonce, c0, t0, &f0);
    dm_session_ratchet_encrypt(&s, &h, a.address, pt, sizeof(pt), nonce, c1, t1, &f1);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(t0, t1, 16)); /* different key -> different tag */
}

/* Shared setup for the failure tests: a healthy, established ratchet session. */
static void make_session(dm_session_t* s, bramble_identity_t* a, bramble_identity_t* b) {
    bramble_identity_t a_eph, b_eph;
    crypto_generate_identity(a);
    crypto_generate_identity(b);
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);
    uint8_t ikm[128];
    TEST_ASSERT_EQUAL(
        0, dm_compute_ikm(a->private_key, a_eph.private_key, b->public_key, b_eph.public_key, ikm));
    memset(s, 0, sizeof(*s));
    s->peer_addr = b->address;
    s->state = DM_STATE_ACTIVE;
    TEST_ASSERT_EQUAL(0, dm_session_ratchet_init_state(s, ikm, a->address, b->address));
}

static void fill_header(bramble_header_t* h, uint32_t dest) {
    memset(h, 0, sizeof(*h));
    h->version = BRAMBLE_VERSION;
    h->type = PKT_TYPE_DATA;
    h->flags = FLAG_ENCRYPT;
    h->hop_limit = 8;
    h->dest_addr = dest;
    h->packet_id = 0x1234;
}

/*
 * Issue #84. dm_ratchet_step used to be void and discard both HKDF results, so
 * a failed derivation handed the caller back whatever its stack held. Assert it
 * now reports the failure AND wipes both outputs, so the pre-seeded poison
 * pattern cannot survive to be used as a key.
 */
void test_ratchet_step_reports_hkdf_failure(void) {
    uint8_t ck[32];
    memset(ck, 0x11, 32);
    uint8_t mk[32], ck_next[32];
    memset(mk, 0xAA, 32); /* stand-in for caller stack garbage */
    memset(ck_next, 0xAA, 32);
    hkdf_fail_after(0);
    TEST_ASSERT_EQUAL(-1, dm_ratchet_step(ck, 0, mk, ck_next));
    TEST_ASSERT_TRUE(all_zero(mk, 32));
    TEST_ASSERT_TRUE(all_zero(ck_next, 32));

    /* And the partial case: the message key derives fine, the chain key does
     * not. Both must still be wiped, never half-committed. */
    memset(mk, 0xAA, 32);
    memset(ck_next, 0xAA, 32);
    hkdf_fail_after(1);
    TEST_ASSERT_EQUAL(-1, dm_ratchet_step(ck, 0, mk, ck_next));
    TEST_ASSERT_TRUE(all_zero(mk, 32));
    TEST_ASSERT_TRUE(all_zero(ck_next, 32));
}

/*
 * Issue #84, the core corruption: a failed step inside ratchet_encrypt must not
 * commit ck_next into the send chain. Snapshot the chain, force the failure,
 * and assert the send chain key and index are byte-identical afterwards (a
 * clean retryable no-send), not advanced to a garbage value.
 */
void test_encrypt_does_not_commit_corrupt_chain(void) {
    dm_session_t s;
    bramble_identity_t a, b;
    make_session(&s, &a, &b);
    uint8_t ck_before[32];
    memcpy(ck_before, s.ratchet.send.ck, 32);
    uint16_t idx_before = s.ratchet.send.index;

    bramble_header_t h;
    fill_header(&h, b.address);
    const uint8_t pt[] = "hi";
    uint8_t nonce[12];
    memset(nonce, 0x01, 12);
    uint8_t ct[DM_RATCHET_HEADER_SIZE + sizeof(pt)];
    uint8_t tag[16];
    size_t flen = 0;

    hkdf_fail_after(0);
    TEST_ASSERT_NOT_EQUAL(
        0, dm_session_ratchet_encrypt(&s, &h, a.address, pt, sizeof(pt), nonce, ct, tag, &flen));
    TEST_ASSERT_EQUAL_MEMORY(ck_before, s.ratchet.send.ck, 32);
    TEST_ASSERT_EQUAL_UINT16(idx_before, s.ratchet.send.index);

    /* The chain is intact, so a retry once HKDF is healthy still works. */
    hkdf_fail_off();
    TEST_ASSERT_EQUAL(
        0, dm_session_ratchet_encrypt(&s, &h, a.address, pt, sizeof(pt), nonce, ct, tag, &flen));
    TEST_ASSERT_EQUAL_UINT16(idx_before + 1, s.ratchet.send.index);
}

/*
 * Issue #84: a failed derivation during initial chain setup must not leave the
 * session marked valid. The slot is pre-poisoned so that the old behaviour
 * (install unconditionally, set both valid bits) would be caught: valid would
 * read 1 with a chain key of 0xAA garbage.
 */
void test_init_state_failure_leaves_session_invalid(void) {
    bramble_identity_t a, b, a_eph, b_eph;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);
    uint8_t ikm[128];
    TEST_ASSERT_EQUAL(
        0, dm_compute_ikm(a.private_key, a_eph.private_key, b.public_key, b_eph.public_key, ikm));

    /* n = 0 fails the root derivation, n = 1 fails the chain pair after the
     * root succeeded (the partially-derived case). Both must fail closed. */
    for (int n = 0; n <= 1; n++) {
        dm_session_t s;
        memset(&s, 0xAA, sizeof(s)); /* poison: nothing here may survive */
        s.peer_addr = b.address;
        s.ke_epoch = 0;
        s.state = DM_STATE_ACTIVE;
        hkdf_fail_after(n);
        TEST_ASSERT_EQUAL(-1, dm_session_ratchet_init_state(&s, ikm, a.address, b.address));
        hkdf_fail_off();
        TEST_ASSERT_EQUAL_UINT8(0, s.ratchet.send.valid);
        TEST_ASSERT_EQUAL_UINT8(0, s.ratchet.recv.valid);
        TEST_ASSERT_TRUE(all_zero(s.ratchet.send.ck, 32));
        TEST_ASSERT_TRUE(all_zero(s.ratchet.recv.ck, 32));
        TEST_ASSERT_TRUE(all_zero(s.ratchet.rk, 32));
        TEST_ASSERT_TRUE(all_zero(s.session_key, 32));

        /* And the invalid session refuses to encrypt: no garbage-keyed frame
         * ever reaches the air. */
        bramble_header_t h;
        fill_header(&h, b.address);
        const uint8_t pt[] = "hi";
        uint8_t nonce[12];
        memset(nonce, 0x01, 12);
        uint8_t ct[DM_RATCHET_HEADER_SIZE + sizeof(pt)];
        uint8_t tag[16];
        size_t flen = 0;
        TEST_ASSERT_NOT_EQUAL(0, dm_session_ratchet_encrypt(&s, &h, a.address, pt, sizeof(pt),
                                                            nonce, ct, tag, &flen));
    }
}

/*
 * Issue #84: a failed derivation during an epoch bump must not install a
 * garbage root or chain pair. It fails closed by wiping the ratchet (see the
 * rationale on dm_session_epoch_bump: the peer has already moved to the new
 * epoch, so silently keeping the old chains would be a one-sided desync and
 * permanent silent loss). Assert both chains end invalid and the session
 * refuses to encrypt, which is what routes the peer into a re-handshake.
 */
void test_epoch_bump_failure_leaves_session_unusable(void) {
    uint8_t dh[32];
    memset(dh, 0x5A, 32);
    for (int n = 0; n <= 1; n++) {
        dm_session_t s;
        bramble_identity_t a, b;
        make_session(&s, &a, &b);
        TEST_ASSERT_EQUAL_UINT8(1, s.ratchet.send.valid); /* healthy to begin with */

        hkdf_fail_after(n);
        TEST_ASSERT_EQUAL(-1, dm_session_epoch_bump(&s, dh, a.address, b.address, 1));
        hkdf_fail_off();

        TEST_ASSERT_EQUAL_UINT8(0, s.ratchet.send.valid);
        TEST_ASSERT_EQUAL_UINT8(0, s.ratchet.recv.valid);
        TEST_ASSERT_EQUAL_UINT8(0, s.ratchet.prev_recv.valid);
        TEST_ASSERT_TRUE(all_zero(s.ratchet.rk, 32));
        TEST_ASSERT_TRUE(all_zero(s.ratchet.send.ck, 32));

        bramble_header_t h;
        fill_header(&h, b.address);
        const uint8_t pt[] = "hi";
        uint8_t nonce[12];
        memset(nonce, 0x01, 12);
        uint8_t ct[DM_RATCHET_HEADER_SIZE + sizeof(pt)];
        uint8_t tag[16];
        size_t flen = 0;
        TEST_ASSERT_NOT_EQUAL(0, dm_session_ratchet_encrypt(&s, &h, a.address, pt, sizeof(pt),
                                                            nonce, ct, tag, &flen));
        /* Receives fail too, which is the signal the mesh caller maps to its
         * rate-limited desync-heal re-handshake. */
        size_t pt_len = 0;
        uint8_t out[64];
        TEST_ASSERT_EQUAL(DM_DECRYPT_FAIL,
                          dm_session_ratchet_decrypt(&s, &h, b.address, nonce, ct,
                                                     DM_RATCHET_HEADER_SIZE + sizeof(pt), tag, out,
                                                     &pt_len));
    }
}

/*
 * Issue #84 receive side: a failed derivation mid-walk must leave the receive
 * chain key and index untouched and report DM_DECRYPT_FAIL, never advance the
 * chain to a garbage value.
 */
void test_decrypt_does_not_commit_corrupt_chain(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b, a_eph, b_eph;
    crypto_generate_identity(&a);
    crypto_generate_identity(&b);
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);
    uint8_t ikm[128];
    TEST_ASSERT_EQUAL(
        0, dm_compute_ikm(a.private_key, a_eph.private_key, b.public_key, b_eph.public_key, ikm));
    memset(&sa, 0, sizeof(sa));
    memset(&sb, 0, sizeof(sb));
    sa.state = sb.state = DM_STATE_ACTIVE;
    TEST_ASSERT_EQUAL(0, dm_session_ratchet_init_state(&sa, ikm, a.address, b.address));
    TEST_ASSERT_EQUAL(0, dm_session_ratchet_init_state(&sb, ikm, b.address, a.address));

    bramble_header_t h;
    fill_header(&h, b.address);
    const uint8_t pt[] = "hello";
    uint8_t nonce[12];
    memset(nonce, 0x07, 12);
    uint8_t ct[DM_RATCHET_HEADER_SIZE + sizeof(pt)];
    uint8_t tag[16];
    size_t flen = 0;
    TEST_ASSERT_EQUAL(
        0, dm_session_ratchet_encrypt(&sa, &h, a.address, pt, sizeof(pt), nonce, ct, tag, &flen));

    uint8_t ck_before[32];
    memcpy(ck_before, sb.ratchet.recv.ck, 32);
    uint16_t idx_before = sb.ratchet.recv.index;

    uint8_t out[64];
    size_t pt_len = 0;
    hkdf_fail_after(0);
    TEST_ASSERT_EQUAL(DM_DECRYPT_FAIL, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce, ct,
                                                                  flen, tag, out, &pt_len));
    hkdf_fail_off();
    TEST_ASSERT_EQUAL_MEMORY(ck_before, sb.ratchet.recv.ck, 32);
    TEST_ASSERT_EQUAL_UINT16(idx_before, sb.ratchet.recv.index);

    /* Chain intact: the same frame decrypts once HKDF is healthy again. */
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce, ct, flen,
                                                                tag, out, &pt_len));
    TEST_ASSERT_EQUAL(sizeof(pt), pt_len);
    TEST_ASSERT_EQUAL_MEMORY(pt, out, sizeof(pt));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rk0_equals_legacy_session_key);
    RUN_TEST(test_ratchet_init_kat);
    RUN_TEST(test_chain_advances_and_mk_changes);
    RUN_TEST(test_no_key_nonce_reuse);
    RUN_TEST(test_dh_ratchet_advances_root);
    RUN_TEST(test_ratchet_encrypt_frames_header);
    RUN_TEST(test_ratchet_encrypt_advances_key);
    RUN_TEST(test_ratchet_step_reports_hkdf_failure);
    RUN_TEST(test_encrypt_does_not_commit_corrupt_chain);
    RUN_TEST(test_init_state_failure_leaves_session_invalid);
    RUN_TEST(test_epoch_bump_failure_leaves_session_unusable);
    RUN_TEST(test_decrypt_does_not_commit_corrupt_chain);
    return UNITY_END();
}
