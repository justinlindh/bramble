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
    for (size_t i = 0; i < n; i++) acc |= a[i] ^ b[i];
    return acc == 0;
}

/* broken_dest(4) || broken_next_hop(4), big-endian: the 2 origin-stable
 * fields, exactly excluding reporter_addr and header.packet_id (the two
 * fields every forwarder rewrites on re-origination). */
static void rerr_build_auth_buf(const bramble_rerr_t* r, uint8_t buf[8]) {
    buf[0] = (uint8_t)(r->broken_dest >> 24);
    buf[1] = (uint8_t)(r->broken_dest >> 16);
    buf[2] = (uint8_t)(r->broken_dest >> 8);
    buf[3] = (uint8_t)r->broken_dest;
    buf[4] = (uint8_t)(r->broken_next_hop >> 24);
    buf[5] = (uint8_t)(r->broken_next_hop >> 16);
    buf[6] = (uint8_t)(r->broken_next_hop >> 8);
    buf[7] = (uint8_t)r->broken_next_hop;
}

void rerr_sign(bramble_rerr_t* r) {
    uint8_t buf[8];
    rerr_build_auth_buf(r, buf);
    network_key_mac("bramble-rerr-v2", buf, sizeof(buf), r->auth_hmac);
}

int rerr_verify(const bramble_rerr_t* r) {
    uint8_t buf[8];
    rerr_build_auth_buf(r, buf);
    uint8_t expect[8];
    network_key_mac("bramble-rerr-v2", buf, sizeof(buf), expect);
    return ct_eq(expect, r->auth_hmac, sizeof(expect));
}
