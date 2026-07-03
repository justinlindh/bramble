#include "include/dm_session.h"
#include <string.h>
#include <stdio.h>

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
 * value (memcmp) before concatenating yields an identical ordering on both
 * sides regardless of which party is "A" or "B". DH1 and DH2 need no such
 * fix: X25519(a_eph_priv,b_eph_pub) == X25519(b_eph_priv,a_eph_pub) and
 * likewise for DH2, so both parties already compute the same value in the
 * same slot without any role-dependent naming.
 */
int dm_derive_session_key(const uint8_t my_id_priv[32], const uint8_t my_eph_priv[32],
                          const uint8_t peer_id_pub[32], const uint8_t peer_eph_pub[32],
                          uint32_t addr_a, uint32_t addr_b, uint16_t ke_epoch,
                          uint8_t session_key_out[32]) {
    uint8_t ikm[128];
    if (crypto_x25519_dh(my_eph_priv, peer_eph_pub, ikm + 0) != 0) return -1;
    if (crypto_x25519_dh(my_id_priv,  peer_id_pub,  ikm + 32) != 0) return -1;

    uint8_t cross_a[32]; /* my_eph x peer_id */
    uint8_t cross_b[32]; /* my_id  x peer_eph */
    if (crypto_x25519_dh(my_eph_priv, peer_id_pub,  cross_a) != 0) return -1;
    if (crypto_x25519_dh(my_id_priv,  peer_eph_pub, cross_b) != 0) return -1;

    /* Canonical ordering: min-first by memcmp, so both parties (who compute
     * the same {cross_a, cross_b} pair in swapped local order) agree. */
    if (memcmp(cross_a, cross_b, 32) <= 0) {
        memcpy(ikm + 64, cross_a, 32);
        memcpy(ikm + 96, cross_b, 32);
    } else {
        memcpy(ikm + 64, cross_b, 32);
        memcpy(ikm + 96, cross_a, 32);
    }

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
