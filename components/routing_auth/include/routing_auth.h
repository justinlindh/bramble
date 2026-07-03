#ifndef BRAMBLE_ROUTING_AUTH_H
#define BRAMBLE_ROUTING_AUTH_H
#include "packet.h"

/*
 * Task 3.3 (SEC-H1, STAGED, NOT closed: see network_key.h). Host-compilable
 * home for the routing/reliability control-plane sign+verify helpers
 * (RERR here; ACK/delivery-receipt land in Task 3.5). mesh_task.c is
 * 3800+ ESP-IDF-only lines and is never #include'd by a host test, so any
 * verify function defined there could never be host-tested; this component
 * exists purely so these can be. mesh_task.c includes this header and
 * calls these functions; it defines none of them.
 *
 * Every helper here is forgeable under network_key.h's unprovisioned
 * public-PSK fallback key: none of them close SEC-H1 on their own.
 */

/*
 * Authenticates broken_dest||broken_next_hop with label "bramble-rerr-v2",
 * deliberately excluding reporter_addr and header.packet_id: every
 * forwarder re-originates a RERR (mesh_task.c's send_rerr) with its own
 * reporter_addr and a fresh packet_id while passing broken_dest/
 * broken_next_hop through unchanged, so those two are the only
 * origin-stable fields. rerr_sign fills r->auth_hmac; call it once, right
 * before serializing (both on first detection and on every
 * re-origination, since send_rerr builds a fresh struct each time).
 * rerr_verify recomputes the same MAC and constant-time-compares; returns
 * nonzero (true) iff it matches.
 */
void rerr_sign(bramble_rerr_t* r);
int rerr_verify(const bramble_rerr_t* r);

#endif
