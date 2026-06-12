#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * RPC authorization policy. Pure functions, no platform dependencies,
 * host-testable (test/test_ws_auth.c).
 *
 * Auth is required by default: every device generates a random token on
 * first boot, and WS/BLE clients must present it. The serial CLI is the
 * unauthenticated pairing bootstrap (physical access = trust, see
 * docs/SECURITY-MODEL.md), so the serial path dispatches with
 * authenticated=true.
 *
 * Unauthenticated WS/BLE clients are limited to the tiny identification
 * allowlist below: just enough for a pairing UI to confirm it is talking
 * to a Bramble device and show its firmware/hardware, nothing that reads
 * mesh state or mutates anything.
 */

/* True if `method` may be dispatched at this auth level. */
bool rpc_auth_method_allowed(const char* method, bool authenticated);
