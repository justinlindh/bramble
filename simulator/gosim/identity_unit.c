/*
 * identity_unit.c: separate cgo compilation unit for the per-node identity
 * Phase 3 components. NOT in all.c because routing_auth.c defines a static
 * ct_eq that collides with discovery.c's identically named helper when
 * both land in one translation unit.
 */
#include "../../components/routing_auth/routing_auth.c"
/* identity.c supplies identity_endorsement_verify/_msg + identity_anchor_set,
 * which identity_store.c and bridge.c now use for the trust-anchor endorsement
 * gate (P2). Safe in this TU: its static put_be64/get_be64 do not collide here
 * (packet.c lives in all.c, a different translation unit). */
#include "../../components/identity/identity.c"
#include "../../components/identity/identity_store.c"
