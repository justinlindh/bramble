/*
 * rollcall_unit.c: separate cgo compilation unit for the attested roll-call
 * core. NOT in all.c because rollcall.c defines static put_be32/get_be32
 * helpers that collide with packet.c's identically named ones when both land
 * in one translation unit, the same reason identity_unit.c exists.
 */
#include "../../components/rollcall/rollcall.c"
