#include "unity.h"
#include "dm_session.h"
#include "packet.h"
#include "../components/crypto/crypto_host.c"
#include "../components/dm_session/dm_session.c"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Build two sessions (A sends, B receives) sharing one handshake IKM. */
static void make_pair(dm_session_t* sa, dm_session_t* sb, bramble_identity_t* a,
                      bramble_identity_t* b) {
    bramble_identity_t a_eph, b_eph;
    crypto_generate_identity(a);
    crypto_generate_identity(b);
    crypto_generate_identity(&a_eph);
    crypto_generate_identity(&b_eph);
    uint8_t ikm[128];
    dm_compute_ikm(a->private_key, a_eph.private_key, b->public_key, b_eph.public_key, ikm);
    memset(sa, 0, sizeof(*sa));
    memset(sb, 0, sizeof(*sb));
    sa->peer_addr = b->address;
    sa->state = DM_STATE_ACTIVE;
    sb->peer_addr = a->address;
    sb->state = DM_STATE_ACTIVE;
    dm_session_ratchet_init_state(sa, ikm, a->address, b->address);
    dm_session_ratchet_init_state(sb, ikm, b->address, a->address);
}

static bramble_header_t hdr(uint32_t dst) {
    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION;
    h.type = PKT_TYPE_DATA;
    h.flags = FLAG_ENCRYPT;
    h.hop_limit = 8;
    h.dest_addr = dst;
    h.packet_id = 0xC0DE;
    return h;
}

/* One frame: A encrypts, B decrypts, header stripped, plaintext exact. */
void test_in_order_roundtrip(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b;
    make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    const uint8_t pt[] = "message zero";
    uint8_t nonce[12];
    memset(nonce, 0x01, 12);
    uint8_t ct[DM_RATCHET_HEADER_SIZE + sizeof(pt)];
    uint8_t tag[16];
    size_t flen;
    TEST_ASSERT_EQUAL(
        0, dm_session_ratchet_encrypt(&sa, &h, a.address, pt, sizeof(pt), nonce, ct, tag, &flen));
    uint8_t out[64];
    size_t olen;
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce, ct, flen,
                                                                tag, out, &olen));
    TEST_ASSERT_EQUAL(sizeof(pt), olen);
    TEST_ASSERT_EQUAL_MEMORY(pt, out, sizeof(pt));
}

/* A long in-order run round-trips: every frame decrypts to its own payload and
 * the receive cursor tracks the sender exactly (no skip cache involved). */
void test_sequential_roundtrip(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b;
    make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    for (int i = 0; i < 10; i++) {
        uint8_t nonce[12];
        memset(nonce, 0x40 + i, 12);
        uint8_t pt[12];
        memset(pt, 'A' + i, sizeof(pt));
        uint8_t ct[DM_RATCHET_HEADER_SIZE + sizeof(pt)];
        uint8_t tag[16];
        size_t flen;
        TEST_ASSERT_EQUAL(0, dm_session_ratchet_encrypt(&sa, &h, a.address, pt, sizeof(pt), nonce,
                                                        ct, tag, &flen));
        uint8_t out[16];
        size_t olen;
        TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce, ct,
                                                                    flen, tag, out, &olen));
        TEST_ASSERT_EQUAL(sizeof(pt), olen);
        TEST_ASSERT_EQUAL_MEMORY(pt, out, sizeof(pt));
    }
    TEST_ASSERT_EQUAL_UINT16(10, sb.ratchet.recv.index);
}

/* Deliver 0,1,2,3 out of order (2,0,3,1), all within MAX_SKIP: all decrypt. */
void test_out_of_order_within_bound(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b;
    make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    uint8_t ct[4][DM_RATCHET_HEADER_SIZE + 8];
    uint8_t tag[4][16];
    size_t flen[4];
    uint8_t nonce[4][12];
    for (int i = 0; i < 4; i++) {
        memset(nonce[i], 0x10 + i, 12);
        uint8_t pt[8];
        memset(pt, 'a' + i, 8);
        dm_session_ratchet_encrypt(&sa, &h, a.address, pt, 8, nonce[i], ct[i], tag[i], &flen[i]);
    }
    int order[4] = {2, 0, 3, 1};
    for (int k = 0; k < 4; k++) {
        int i = order[k];
        uint8_t out[16];
        size_t olen;
        TEST_ASSERT_EQUAL(DM_DECRYPT_OK,
                          dm_session_ratchet_decrypt(&sb, &h, a.address, nonce[i], ct[i], flen[i],
                                                     tag[i], out, &olen));
        TEST_ASSERT_EQUAL(8, olen);
        uint8_t exp[8];
        memset(exp, 'a' + i, 8);
        TEST_ASSERT_EQUAL_MEMORY(exp, out, 8);
    }
}

/* A jumps to index next+DM_MAX_SKIP+1 (one past the bound): B refuses
 * (DM_DECRYPT_TOO_FAR) without deriving anything, no crash. This is the DoS
 * bound and the desync-heal trigger. The tested frame is the LAST one encrypted,
 * whose cleartext index is DM_MAX_SKIP+1 while B's next is still 0. */
void test_beyond_max_skip_refused(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b;
    make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    uint8_t ct[DM_RATCHET_HEADER_SIZE + 4];
    uint8_t tag[16];
    size_t flen;
    uint8_t nonce[12];
    for (int i = 0; i <= DM_MAX_SKIP + 1; i++) { /* indices 0 .. DM_MAX_SKIP+1 */
        memset(nonce, 0x20 + (i & 0x1F), 12);
        uint8_t pt[4] = {1, 2, 3, 4};
        dm_session_ratchet_encrypt(&sa, &h, a.address, pt, 4, nonce, ct, tag, &flen);
    }
    uint8_t out[16];
    size_t olen;
    TEST_ASSERT_EQUAL(DM_DECRYPT_TOO_FAR, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce, ct,
                                                                     flen, tag, out, &olen));
    TEST_ASSERT_EQUAL_UINT16(0, sb.ratchet.recv.index); /* recv chain not advanced past bound */
}

/* Exactly at the bound: index == next + DM_MAX_SKIP is accepted (the largest
 * gap the receiver will derive across). Confirms the boundary is inclusive and
 * that both ends of the freshly-filled skip cache remain decryptable. */
void test_at_max_skip_boundary_accepted(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b;
    make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    const int last = DM_MAX_SKIP; /* indices 0 .. DM_MAX_SKIP, next is 0 */
    uint8_t ct[DM_MAX_SKIP + 1][DM_RATCHET_HEADER_SIZE + 4];
    uint8_t tag[DM_MAX_SKIP + 1][16];
    size_t flen[DM_MAX_SKIP + 1];
    uint8_t nonce[DM_MAX_SKIP + 1][12];
    for (int i = 0; i <= last; i++) {
        memset(nonce[i], 0x60 + (i & 0x1F), 12);
        uint8_t pt[4] = {(uint8_t)i, 2, 3, 4};
        dm_session_ratchet_encrypt(&sa, &h, a.address, pt, 4, nonce[i], ct[i], tag[i], &flen[i]);
    }
    uint8_t out[16];
    size_t olen;
    /* The frame at the exact bound decrypts and advances next past it. */
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK,
                      dm_session_ratchet_decrypt(&sb, &h, a.address, nonce[last], ct[last],
                                                 flen[last], tag[last], out, &olen));
    TEST_ASSERT_EQUAL_UINT16(last + 1, sb.ratchet.recv.index);
    /* Both ends of the range that got cached in the walk are still decryptable. */
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce[0], ct[0],
                                                                flen[0], tag[0], out, &olen));
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK,
                      dm_session_ratchet_decrypt(&sb, &h, a.address, nonce[last - 1], ct[last - 1],
                                                 flen[last - 1], tag[last - 1], out, &olen));
}

/* A cached skipped key is single-use: deliver index 2 first (caches 0,1), then
 * the straggler 0 decrypts once and is evicted, and a replay of 0 is refused. */
void test_straggler_single_use_eviction(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b;
    make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    uint8_t ct[3][DM_RATCHET_HEADER_SIZE + 4];
    uint8_t tag[3][16];
    size_t flen[3];
    uint8_t nonce[3][12];
    for (int i = 0; i < 3; i++) {
        memset(nonce[i], 0x30 + i, 12);
        uint8_t pt[4] = {(uint8_t)i, 9, 9, 9};
        dm_session_ratchet_encrypt(&sa, &h, a.address, pt, 4, nonce[i], ct[i], tag[i], &flen[i]);
    }
    uint8_t out[16];
    size_t olen;
    /* Deliver 2 first: caches skipped keys for 0 and 1, next -> 3. */
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce[2], ct[2],
                                                                flen[2], tag[2], out, &olen));
    TEST_ASSERT_EQUAL_UINT16(3, sb.ratchet.recv.index);
    /* Straggler 0: cache hit, decrypts, then the entry is evicted. */
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce[0], ct[0],
                                                                flen[0], tag[0], out, &olen));
    TEST_ASSERT_EQUAL(0, out[0]);
    /* Replay of the just-consumed straggler: no longer cached -> refused. */
    TEST_ASSERT_EQUAL(DM_DECRYPT_TOO_FAR,
                      dm_session_ratchet_decrypt(&sb, &h, a.address, nonce[0], ct[0], flen[0],
                                                 tag[0], out, &olen));
    /* Straggler 1 is independent and still decrypts. */
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce[1], ct[1],
                                                                flen[1], tag[1], out, &olen));
    TEST_ASSERT_EQUAL(1, out[0]);
}

/* Replay of an index that was consumed IN ORDER (never cached) is refused: the
 * cursor is past it and the skip cache never held it. */
void test_replay_consumed_in_order(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b;
    make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    uint8_t nonce[12];
    memset(nonce, 0x50, 12);
    uint8_t pt[] = "hello";
    uint8_t ct[DM_RATCHET_HEADER_SIZE + sizeof(pt)];
    uint8_t tag[16];
    size_t flen;
    dm_session_ratchet_encrypt(&sa, &h, a.address, pt, sizeof(pt), nonce, ct, tag, &flen);
    uint8_t out[32];
    size_t olen;
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce, ct, flen,
                                                                tag, out, &olen));
    TEST_ASSERT_EQUAL_UINT16(1, sb.ratchet.recv.index);
    /* Same frame again: index 0 < next(1), not in skip cache -> refused. */
    TEST_ASSERT_EQUAL(DM_DECRYPT_TOO_FAR, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce, ct,
                                                                     flen, tag, out, &olen));
    TEST_ASSERT_EQUAL_UINT16(1, sb.ratchet.recv.index); /* cursor unmoved by the replay */
}

/* A forged/corrupted frame at the head of the chain fails the tag and leaves the
 * receive chain untouched (DM_DECRYPT_FAIL, not TOO_FAR, and next unchanged). */
void test_forged_frame_leaves_chain_untouched(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b;
    make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    uint8_t nonce[12];
    memset(nonce, 0x70, 12);
    uint8_t pt[] = "authentic";
    uint8_t ct[DM_RATCHET_HEADER_SIZE + sizeof(pt)];
    uint8_t tag[16];
    size_t flen;
    dm_session_ratchet_encrypt(&sa, &h, a.address, pt, sizeof(pt), nonce, ct, tag, &flen);
    tag[0] ^= 0xFF; /* corrupt the GCM tag */
    uint8_t out[32];
    size_t olen;
    TEST_ASSERT_EQUAL(DM_DECRYPT_FAIL, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce, ct,
                                                                  flen, tag, out, &olen));
    TEST_ASSERT_EQUAL_UINT16(0, sb.ratchet.recv.index); /* chain not advanced on a bad tag */
    /* The genuine frame still decrypts afterward: state really was untouched. */
    tag[0] ^= 0xFF;
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce, ct, flen,
                                                                tag, out, &olen));
    TEST_ASSERT_EQUAL_MEMORY(pt, out, sizeof(pt));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_in_order_roundtrip);
    RUN_TEST(test_sequential_roundtrip);
    RUN_TEST(test_out_of_order_within_bound);
    RUN_TEST(test_beyond_max_skip_refused);
    RUN_TEST(test_at_max_skip_boundary_accepted);
    RUN_TEST(test_straggler_single_use_eviction);
    RUN_TEST(test_replay_consumed_in_order);
    RUN_TEST(test_forged_frame_leaves_chain_untouched);
    return UNITY_END();
}
