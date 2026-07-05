#include "include/routing_auth.h"
#include "network_key.h"
#include <string.h>

/*
 * Constant-time equality (tag comparison), same OR-accumulate pattern used
 * elsewhere in this codebase for MAC/tag checks (dm_session.c, discovery.c):
 * no early exit, so comparing a computed MAC against an attacker-supplied
 * one never leaks how many leading bytes matched.
 */
static int ct_eq(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++)
        acc |= a[i] ^ b[i];
    return acc == 0;
}

/* reporter_addr(4) || broken_dest(4) || broken_next_hop(4) || seq(6),
 * big-endian for the multi-byte fields: was broken_dest||broken_next_hop
 * only (8 bytes); ws 1.3b adds reporter_addr and seq (now 18 bytes).
 * reporter_addr is safe to cover because every forwarder re-signs with
 * its OWN reporter_addr on every re-origination (send_rerr rebuilds the
 * whole struct and calls rerr_sign again each time); only
 * header.packet_id remains uncovered, the one field every forwarder still
 * rewrites without re-deriving a value this MAC could pin. */
static void rerr_build_auth_buf(const bramble_rerr_t* r, uint8_t buf[18]) {
    buf[0] = (uint8_t)(r->reporter_addr >> 24);
    buf[1] = (uint8_t)(r->reporter_addr >> 16);
    buf[2] = (uint8_t)(r->reporter_addr >> 8);
    buf[3] = (uint8_t)r->reporter_addr;
    buf[4] = (uint8_t)(r->broken_dest >> 24);
    buf[5] = (uint8_t)(r->broken_dest >> 16);
    buf[6] = (uint8_t)(r->broken_dest >> 8);
    buf[7] = (uint8_t)r->broken_dest;
    buf[8] = (uint8_t)(r->broken_next_hop >> 24);
    buf[9] = (uint8_t)(r->broken_next_hop >> 16);
    buf[10] = (uint8_t)(r->broken_next_hop >> 8);
    buf[11] = (uint8_t)r->broken_next_hop;
    memcpy(buf + 12, r->seq, 6);
}

int rerr_sign(bramble_rerr_t* r) {
    uint8_t buf[18];
    rerr_build_auth_buf(r, buf);
    /* Fail-closed: when unprovisioned network_key_mac writes the all-zero
     * sentinel and returns nonzero; propagate so the caller does not emit a
     * frame carrying a bogus MAC. */
    return network_key_mac("bramble-rerr-v2", buf, sizeof(buf), r->auth_hmac);
}

int rerr_verify(const bramble_rerr_t* r) {
    uint8_t buf[18];
    rerr_build_auth_buf(r, buf);
    uint8_t expect[8];
    /* CRITICAL: reject BEFORE the constant-time compare. Unprovisioned
     * network_key_mac emits the all-zero sentinel, so comparing it against a
     * received all-zero MAC would otherwise ACCEPT a forgery. */
    if (network_key_mac("bramble-rerr-v2", buf, sizeof(buf), expect) != 0)
        return 0;
    return ct_eq(expect, r->auth_hmac, sizeof(expect));
}

/* src_addr(4) || ack_packet_id(4) || seq(6), big-endian for the
 * multi-byte fields: the origin-stable fields, exactly excluding
 * relay_path/hop_count/header.hop_limit (the fields forward_ack mutates
 * on every relay hop). seq (ws 1.3b) is origin-stable like the rest:
 * forward_ack carries it through unchanged, so it belongs in the same
 * coverage set. */
static void ack_build_auth_buf(const bramble_ack_t* a, uint8_t buf[14]) {
    buf[0] = (uint8_t)(a->src_addr >> 24);
    buf[1] = (uint8_t)(a->src_addr >> 16);
    buf[2] = (uint8_t)(a->src_addr >> 8);
    buf[3] = (uint8_t)a->src_addr;
    buf[4] = (uint8_t)(a->ack_packet_id >> 24);
    buf[5] = (uint8_t)(a->ack_packet_id >> 16);
    buf[6] = (uint8_t)(a->ack_packet_id >> 8);
    buf[7] = (uint8_t)a->ack_packet_id;
    memcpy(buf + 8, a->seq, 6);
}

int ack_sign(bramble_ack_t* a) {
    uint8_t buf[14];
    ack_build_auth_buf(a, buf);
    return network_key_mac("bramble-ack-v2", buf, sizeof(buf), a->auth_hmac);
}

int ack_verify(const bramble_ack_t* a) {
    uint8_t buf[14];
    ack_build_auth_buf(a, buf);
    uint8_t expect[8];
    /* Reject before compare (see rerr_verify): unprovisioned cannot verify. */
    if (network_key_mac("bramble-ack-v2", buf, sizeof(buf), expect) != 0)
        return 0;
    return ct_eq(expect, a->auth_hmac, sizeof(expect));
}

/* src_addr(4) || orig_packet_id(4) || seq(6), big-endian for the
 * multi-byte fields: the origin-stable fields, exactly excluding
 * relay_path/hop_count/header.hop_limit (the fields
 * forward_delivery_receipt mutates on every relay hop). seq (ws 1.3b) is
 * origin-stable like the rest: forward_delivery_receipt carries it
 * through unchanged, so it belongs in the same coverage set. */
static void receipt_build_auth_buf(const bramble_delivery_receipt_t* r, uint8_t buf[14]) {
    buf[0] = (uint8_t)(r->src_addr >> 24);
    buf[1] = (uint8_t)(r->src_addr >> 16);
    buf[2] = (uint8_t)(r->src_addr >> 8);
    buf[3] = (uint8_t)r->src_addr;
    buf[4] = (uint8_t)(r->orig_packet_id >> 24);
    buf[5] = (uint8_t)(r->orig_packet_id >> 16);
    buf[6] = (uint8_t)(r->orig_packet_id >> 8);
    buf[7] = (uint8_t)r->orig_packet_id;
    memcpy(buf + 8, r->seq, 6);
}

int receipt_sign(bramble_delivery_receipt_t* r) {
    uint8_t buf[14];
    receipt_build_auth_buf(r, buf);
    return network_key_mac("bramble-receipt-v2", buf, sizeof(buf), r->auth_hmac);
}

int receipt_verify(const bramble_delivery_receipt_t* r) {
    uint8_t buf[14];
    receipt_build_auth_buf(r, buf);
    uint8_t expect[8];
    /* Reject before compare (see rerr_verify): unprovisioned cannot verify. */
    if (network_key_mac("bramble-receipt-v2", buf, sizeof(buf), expect) != 0)
        return 0;
    return ct_eq(expect, r->auth_hmac, sizeof(expect));
}

/* DATA origin authentication (Task 4-fix F1). The MAC covers exactly the
 * bytes bramble_build_aead_aad emits -- masked header (hop_limit zeroed) ||
 * LE src_addr, HEADER_SIZE + 4 bytes -- which structurally excludes the two
 * relay-mutable fields (prev_hop lives further out in the wire envelope and
 * is never copied into this buffer; hop_limit is masked to zero here). Reuse
 * of that helper guarantees the sign path and the AEAD AAD masking cannot
 * diverge. */
static void data_build_auth_buf(const bramble_header_t* h, uint32_t src_addr,
                                uint8_t buf[HEADER_SIZE + 4]) {
    bramble_build_aead_aad(h, src_addr, buf, HEADER_SIZE + 4);
}

int data_auth_sign(const bramble_header_t* h, uint32_t src_addr, uint8_t out[8]) {
    uint8_t buf[HEADER_SIZE + 4];
    data_build_auth_buf(h, src_addr, buf);
    return network_key_mac("bramble-data-v1", buf, sizeof(buf), out);
}

int data_auth_verify(const bramble_header_t* h, uint32_t src_addr, const uint8_t hmac[8]) {
    uint8_t buf[HEADER_SIZE + 4];
    data_build_auth_buf(h, src_addr, buf);
    uint8_t expect[8];
    /* Reject before compare (see rerr_verify): unprovisioned cannot verify,
     * so a keyless attacker's all-zero MAC never lays a reverse-route
     * breadcrumb. */
    if (network_key_mac("bramble-data-v1", buf, sizeof(buf), expect) != 0)
        return 0;
    return ct_eq(expect, hmac, sizeof(expect));
}

/* src_addr(4, BE) || x25519_pub(32) || ed25519_pub(32) || sig(64) || seq(6)
 * = 138 bytes: every origin-stable field of the attestation, excluding
 * only the header (hop_limit is the one relay-mutated field; packet_id is
 * per-send and already outside every stable authenticator on this frame,
 * matching the Ed25519 sig's own header exclusion in packet.c). */
static void ident_relay_build_auth_buf(const bramble_identity_attestation_t* a, uint8_t buf[138]) {
    buf[0] = (uint8_t)(a->src_addr >> 24);
    buf[1] = (uint8_t)(a->src_addr >> 16);
    buf[2] = (uint8_t)(a->src_addr >> 8);
    buf[3] = (uint8_t)a->src_addr;
    memcpy(buf + 4, a->x25519_pub, 32);
    memcpy(buf + 36, a->ed25519_pub, 32);
    memcpy(buf + 68, a->sig, 64);
    memcpy(buf + 132, a->seq, 6);
}

int ident_relay_sign(bramble_identity_attestation_t* a) {
    uint8_t buf[138];
    ident_relay_build_auth_buf(a, buf);
    return network_key_mac("bramble-ident-relay-v1", buf, sizeof(buf), a->auth_hmac);
}

int ident_relay_verify(const bramble_identity_attestation_t* a) {
    uint8_t buf[138];
    ident_relay_build_auth_buf(a, buf);
    uint8_t expect[8];
    /* Reject before compare (see rerr_verify): unprovisioned cannot verify,
     * so an unprovisioned relay never propagates an attestation. */
    if (network_key_mac("bramble-ident-relay-v1", buf, sizeof(buf), expect) != 0)
        return 0;
    return ct_eq(expect, a->auth_hmac, sizeof(expect));
}
