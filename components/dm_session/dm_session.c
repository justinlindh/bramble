#include "include/dm_session.h"
#include <string.h>
#include <stdio.h>

/*
 * Constant-time big-endian lexicographic "a <= b" over two 32-byte SECRET
 * DH outputs. Every byte pair is processed unconditionally (no early exit,
 * no branch whose target depends on where a and b first differ), unlike
 * memcmp, which typically stops scanning at the first differing byte and
 * so leaks that byte's position through timing. Returns 1 if a <= b, 0
 * otherwise; ties (a == b) count as <=. The only property agreement needs
 * is that both parties apply the identical deterministic total order, which
 * this does regardless of timing.
 */
static int ct_le32(const uint8_t a[32], const uint8_t b[32]) {
    uint32_t decided = 0; /* 1 once a more-significant byte already differed */
    uint32_t a_le_b = 1;  /* running answer; equal-so-far counts as <= */
    for (int i = 0; i < 32; i++) {
        uint32_t ai = a[i], bi = b[i];
        uint32_t is_gt = ((uint32_t)(bi - ai)) >> 31; /* 1 iff ai > bi */
        uint32_t is_lt = ((uint32_t)(ai - bi)) >> 31; /* 1 iff ai < bi */
        uint32_t is_diff = is_gt | is_lt;
        uint32_t apply = is_diff & (1u - decided);
        uint32_t apply_mask = 0u - apply; /* all-ones if apply else 0 */
        a_le_b = (a_le_b & ~apply_mask) | (is_lt & apply_mask);
        decided = decided | is_diff;
    }
    return (int)a_le_b;
}

/*
 * IKM = DH1(eph,eph) || DH2(id,id) || sorted{DH3(my_eph,peer_id), DH4(my_id,peer_eph)}.
 * RFC section 1 (2026-06-12-crypto-design-r2.md) states the formula as
 * DH1 || DH2 || DH3 || DH4 in "my/peer" terms, without addressing that DH3
 * and DH4 swap between the two parties: X25519 is symmetric, so
 * X25519(a_eph_priv, b_id_pub) == X25519(b_id_priv, a_eph_pub), i.e. party
 * A's DH3 equals party B's DH4, and A's DH4 equals B's DH3. Concatenating
 * "my DH3 then my DH4" per party therefore gives A and B the SAME two
 * 32-byte values but in SWAPPED order, so a naive implementation derives
 * different IKM (and thus different session keys) on each side.
 *
 * Fix: both parties compute the same UNORDERED pair {DH3, DH4} (one party's
 * DH3 is the other's DH4 and vice versa), so sorting that pair by raw byte
 * value before concatenating yields an identical ordering on both sides
 * regardless of which party is "A" or "B". The compare and the placement
 * are both constant-time: cross_a/cross_b are secret DH outputs, so neither
 * the comparison nor which one lands in which IKM slot may depend on secret
 * data through a timing-observable branch. DH1 and DH2 need no such fix:
 * X25519(a_eph_priv,b_eph_pub) == X25519(b_eph_priv,a_eph_pub) and likewise
 * for DH2, so both parties already compute the same value in the same slot
 * without any role-dependent naming.
 */
static int dm_compute_ikm(const uint8_t my_id_priv[32], const uint8_t my_eph_priv[32],
                          const uint8_t peer_id_pub[32], const uint8_t peer_eph_pub[32],
                          uint8_t ikm_out[128]) {
    if (crypto_x25519_dh(my_eph_priv, peer_eph_pub, ikm_out + 0) != 0) return -1;
    if (crypto_x25519_dh(my_id_priv,  peer_id_pub,  ikm_out + 32) != 0) return -1;

    uint8_t cross_a[32]; /* my_eph x peer_id */
    uint8_t cross_b[32]; /* my_id  x peer_eph */
    if (crypto_x25519_dh(my_eph_priv, peer_id_pub,  cross_a) != 0) return -1;
    if (crypto_x25519_dh(my_id_priv,  peer_eph_pub, cross_b) != 0) return -1;

    /* Canonical ordering: min-first, both compare and placement branchless. */
    uint32_t le_mask = 0u - (uint32_t)ct_le32(cross_a, cross_b); /* all-ones if cross_a<=cross_b */
    for (int i = 0; i < 32; i++) {
        ikm_out[64 + i] = (uint8_t)((cross_a[i] & le_mask) | (cross_b[i] & ~le_mask));
        ikm_out[96 + i] = (uint8_t)((cross_b[i] & le_mask) | (cross_a[i] & ~le_mask));
    }
    return 0;
}

int dm_derive_session_key(const uint8_t my_id_priv[32], const uint8_t my_eph_priv[32],
                          const uint8_t peer_id_pub[32], const uint8_t peer_eph_pub[32],
                          uint32_t addr_a, uint32_t addr_b, uint16_t ke_epoch,
                          uint8_t session_key_out[32]) {
    uint8_t ikm[128];
    if (dm_compute_ikm(my_id_priv, my_eph_priv, peer_id_pub, peer_eph_pub, ikm) != 0) return -1;

    uint32_t lo = addr_a < addr_b ? addr_a : addr_b;
    uint32_t hi = addr_a < addr_b ? addr_b : addr_a;
    uint8_t info[10];
    info[0]=(uint8_t)(lo>>24); info[1]=(uint8_t)(lo>>16); info[2]=(uint8_t)(lo>>8); info[3]=(uint8_t)lo;
    info[4]=(uint8_t)(hi>>24); info[5]=(uint8_t)(hi>>16); info[6]=(uint8_t)(hi>>8); info[7]=(uint8_t)hi;
    info[8]=(uint8_t)(ke_epoch & 0xFF); info[9]=(uint8_t)((ke_epoch>>8) & 0xFF);
    const char* salt = "bramble-dm-v2";
    return crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), ikm, sizeof(ikm),
                              info, sizeof(info), session_key_out, 32);
}

int dm_derive_sas(const uint8_t session_ikm[128], char sas_out[8]) {
    const char* salt = "bramble-sas";
    uint8_t okm[4];
    if (crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), session_ikm, 128,
                           NULL, 0, okm, sizeof(okm)) != 0) return -1;
    /* Render as 7 decimal digits, human-comparable. */
    uint32_t v = ((uint32_t)okm[0]<<24)|((uint32_t)okm[1]<<16)|((uint32_t)okm[2]<<8)|okm[3];
    snprintf(sas_out, 8, "%07u", (unsigned)(v % 10000000u));
    return 0;
}
