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

void rerr_sign(bramble_rerr_t* r) {
    uint8_t buf[18];
    rerr_build_auth_buf(r, buf);
    network_key_mac("bramble-rerr-v2", buf, sizeof(buf), r->auth_hmac);
}

int rerr_verify(const bramble_rerr_t* r) {
    uint8_t buf[18];
    rerr_build_auth_buf(r, buf);
    uint8_t expect[8];
    network_key_mac("bramble-rerr-v2", buf, sizeof(buf), expect);
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

void ack_sign(bramble_ack_t* a) {
    uint8_t buf[14];
    ack_build_auth_buf(a, buf);
    network_key_mac("bramble-ack-v2", buf, sizeof(buf), a->auth_hmac);
}

int ack_verify(const bramble_ack_t* a) {
    uint8_t buf[14];
    ack_build_auth_buf(a, buf);
    uint8_t expect[8];
    network_key_mac("bramble-ack-v2", buf, sizeof(buf), expect);
    return ct_eq(expect, a->auth_hmac, sizeof(expect));
}

/* src_addr(4) || orig_packet_id(4), big-endian: the 2 origin-stable
 * fields, exactly excluding relay_path/hop_count/header.hop_limit (the
 * fields forward_delivery_receipt mutates on every relay hop). */
static void receipt_build_auth_buf(const bramble_delivery_receipt_t* r, uint8_t buf[8]) {
    buf[0] = (uint8_t)(r->src_addr >> 24);
    buf[1] = (uint8_t)(r->src_addr >> 16);
    buf[2] = (uint8_t)(r->src_addr >> 8);
    buf[3] = (uint8_t)r->src_addr;
    buf[4] = (uint8_t)(r->orig_packet_id >> 24);
    buf[5] = (uint8_t)(r->orig_packet_id >> 16);
    buf[6] = (uint8_t)(r->orig_packet_id >> 8);
    buf[7] = (uint8_t)r->orig_packet_id;
}

void receipt_sign(bramble_delivery_receipt_t* r) {
    uint8_t buf[8];
    receipt_build_auth_buf(r, buf);
    network_key_mac("bramble-receipt-v2", buf, sizeof(buf), r->auth_hmac);
}

int receipt_verify(const bramble_delivery_receipt_t* r) {
    uint8_t buf[8];
    receipt_build_auth_buf(r, buf);
    uint8_t expect[8];
    network_key_mac("bramble-receipt-v2", buf, sizeof(buf), expect);
    return ct_eq(expect, r->auth_hmac, sizeof(expect));
}
