/*
 * identity_unit.c: separate cgo compilation unit for the per-node identity
 * Phase 3 components. NOT in all.c because routing_auth.c defines a static
 * ct_eq that collides with discovery.c's identically named helper when
 * both land in one translation unit.
 */
#include "../../components/routing_auth/routing_auth.c"
#include "../../components/identity/identity_store.c"
