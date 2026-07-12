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

/* After an epoch bump, in-flight OLD-epoch messages still decrypt during the
 * grace, NEW-epoch messages decrypt, and the recv epoch advanced. */
void test_epoch_transition_grace_then_wipe(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b;
    make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    /* A sends one old-epoch message; B has not received it yet (in flight). */
    const uint8_t old_pt[] = "old-epoch";
    uint8_t onc[12];
    memset(onc, 0x40, 12);
    uint8_t oct[DM_RATCHET_HEADER_SIZE + sizeof(old_pt)];
    uint8_t otag[16];
    size_t oflen;
    dm_session_ratchet_encrypt(&sa, &h, a.address, old_pt, sizeof(old_pt), onc, oct, otag, &oflen);

    uint8_t dh[32];
    memset(dh, 0x77, 32);
    dm_session_epoch_bump(&sa, dh, a.address, b.address, 1);
    dm_session_epoch_bump(&sb, dh, b.address, a.address, 1);

    uint8_t out[32];
    size_t olen;
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, onc, oct, oflen,
                                                                otag, out, &olen)); /* grace */
    TEST_ASSERT_EQUAL_MEMORY(old_pt, out, sizeof(old_pt));

    const uint8_t new_pt[] = "new-epoch";
    uint8_t nnc[12];
    memset(nnc, 0x41, 12);
    uint8_t nct[DM_RATCHET_HEADER_SIZE + sizeof(new_pt)];
    uint8_t ntag[16];
    size_t nflen;
    dm_session_ratchet_encrypt(&sa, &h, a.address, new_pt, sizeof(new_pt), nnc, nct, ntag, &nflen);
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nnc, nct, nflen,
                                                                ntag, out, &olen));
    TEST_ASSERT_EQUAL_MEMORY(new_pt, out, sizeof(new_pt));
    TEST_ASSERT_EQUAL_UINT8(1, sb.ratchet.recv.epoch);
}

/* The root changed: an epoch bump yields a different root (PCS: a captured
 * pre-bump root cannot derive post-bump keys). */
void test_epoch_bump_changes_root(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b;
    make_pair(&sa, &sb, &a, &b);
    uint8_t rk_before[32];
    memcpy(rk_before, sa.ratchet.rk, 32);
    uint8_t dh[32];
    memset(dh, 0x5A, 32);
    dm_session_epoch_bump(&sa, dh, a.address, b.address, 1);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(rk_before, sa.ratchet.rk, 32));
}

/* Both peers bump to the same epoch with the same DH and land on the same root
 * (the DH-ratchet must converge or the session dies). New-epoch DMs then flow
 * bidirectionally. */
void test_epoch_bump_converges_both_sides(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b;
    make_pair(&sa, &sb, &a, &b);
    uint8_t dh[32];
    memset(dh, 0x33, 32);
    dm_session_epoch_bump(&sa, dh, a.address, b.address, 1);
    dm_session_epoch_bump(&sb, dh, b.address, a.address, 1);
    TEST_ASSERT_EQUAL_MEMORY(sa.ratchet.rk, sb.ratchet.rk, 32);

    /* A -> B on the new epoch. */
    bramble_header_t hb = hdr(b.address);
    const uint8_t m1[] = "a-to-b";
    uint8_t n1[12];
    memset(n1, 0x51, 12);
    uint8_t c1[DM_RATCHET_HEADER_SIZE + sizeof(m1)];
    uint8_t t1[16];
    size_t f1;
    dm_session_ratchet_encrypt(&sa, &hb, a.address, m1, sizeof(m1), n1, c1, t1, &f1);
    uint8_t out[32];
    size_t olen;
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK,
                      dm_session_ratchet_decrypt(&sb, &hb, a.address, n1, c1, f1, t1, out, &olen));
    TEST_ASSERT_EQUAL_MEMORY(m1, out, sizeof(m1));

    /* B -> A on the new epoch. */
    bramble_header_t ha = hdr(a.address);
    const uint8_t m2[] = "b-to-a";
    uint8_t n2[12];
    memset(n2, 0x52, 12);
    uint8_t c2[DM_RATCHET_HEADER_SIZE + sizeof(m2)];
    uint8_t t2[16];
    size_t f2;
    dm_session_ratchet_encrypt(&sb, &ha, b.address, m2, sizeof(m2), n2, c2, t2, &f2);
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK,
                      dm_session_ratchet_decrypt(&sa, &ha, b.address, n2, c2, f2, t2, out, &olen));
    TEST_ASSERT_EQUAL_MEMORY(m2, out, sizeof(m2));
}

/* Once DM_EPOCH_GRACE_MSGS new-epoch messages have been seen, the previous
 * epoch's chain is wiped: a still-in-flight old-epoch straggler no longer
 * decrypts (this wipe is what delivers PCS). */
void test_prev_epoch_wiped_after_grace(void) {
    dm_session_t sa, sb;
    bramble_identity_t a, b;
    make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);

    /* A encrypts one old-epoch straggler, held back (in flight). */
    const uint8_t old_pt[] = "straggler";
    uint8_t onc[12];
    memset(onc, 0x60, 12);
    uint8_t oct[DM_RATCHET_HEADER_SIZE + sizeof(old_pt)];
    uint8_t otag[16];
    size_t oflen;
    dm_session_ratchet_encrypt(&sa, &h, a.address, old_pt, sizeof(old_pt), onc, oct, otag, &oflen);

    uint8_t dh[32];
    memset(dh, 0x77, 32);
    dm_session_epoch_bump(&sa, dh, a.address, b.address, 1);
    dm_session_epoch_bump(&sb, dh, b.address, a.address, 1);

    /* Deliver DM_EPOCH_GRACE_MSGS new-epoch messages in order: the grace expires. */
    uint8_t out[32];
    size_t olen;
    for (int i = 0; i < DM_EPOCH_GRACE_MSGS; i++) {
        uint8_t pt[8];
        memset(pt, 'a' + (i & 15), 8);
        uint8_t nnc[12];
        memset(nnc, 0x80 + i, 12);
        uint8_t nct[DM_RATCHET_HEADER_SIZE + 8];
        uint8_t ntag[16];
        size_t nflen;
        dm_session_ratchet_encrypt(&sa, &h, a.address, pt, 8, nnc, nct, ntag, &nflen);
        TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nnc, nct,
                                                                    nflen, ntag, out, &olen));
    }

    /* The old-epoch straggler now fails: prev chain wiped (PCS). It degrades to
     * the desync-heal signal, not a silent success. */
    int rc = dm_session_ratchet_decrypt(&sb, &h, a.address, onc, oct, oflen, otag, out, &olen);
    TEST_ASSERT_NOT_EQUAL(DM_DECRYPT_OK, rc);
    TEST_ASSERT_EQUAL_UINT8(0, sb.ratchet.prev_recv.valid);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_epoch_transition_grace_then_wipe);
    RUN_TEST(test_epoch_bump_changes_root);
    RUN_TEST(test_epoch_bump_converges_both_sides);
    RUN_TEST(test_prev_epoch_wiped_after_grace);
    return UNITY_END();
}
