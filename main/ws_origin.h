#pragma once

#include <stdbool.h>

/*
 * WebSocket Origin allowlist decision (SEC-H3, CSWSH).
 *
 * Pure function, no platform dependencies, host-testable
 * (test/test_ws_origin.c). Policy:
 *
 *   - No Origin header: ALLOW. Non-browser clients (CLI, SDKs) omit it
 *     and are not subject to cross-site WebSocket hijacking; a browser
 *     always sends it on WS upgrades.
 *   - Same-origin: ALLOW. The Origin host equals the Host header's host
 *     (case-insensitive, any port, any scheme). This covers the device's
 *     IP and any hostname the client reached it by (mDNS .local included)
 *     without the device needing to know its own names.
 *   - Configured extras: ALLOW. `extra_origins` is a comma-separated list
 *     of full origins (e.g. "https://app.example.com") stored in NVS and
 *     settable only via authenticated RPC. Compared case-insensitively to
 *     the whole Origin value, ignoring a trailing slash.
 *   - Everything else (including the literal "null" Origin from sandboxed
 *     iframes and file:// pages, and malformed values): REJECT.
 *
 * `origin` and `extra_origins` may be NULL or empty. `host_hdr` is the raw
 * Host header value ("ip[:port]" or "name[:port]").
 */
bool ws_origin_allowed(const char* origin, const char* host_hdr, const char* extra_origins);

/*
 * CSRF decision for the Wi-Fi config POST (a CORS-simple form target:
 * any web page can auto-submit it, so a foreign page could rewrite the
 * device's Wi-Fi credentials and reboot it onto an attacker AP).
 *
 * Policy, simplest correct given the setup portal is same-origin by
 * construction (the device serves the form itself):
 *   - Origin header present: full ws_origin_allowed() policy
 *     (same-origin or configured extras).
 *   - No Origin but Referer present: same-origin only. Referers carry a
 *     path, so the exact-match extras list can never match one; a
 *     cross-origin browser that legitimately posts this form does not
 *     exist.
 *   - Neither header: allow (curl and scripts; they are not CSRF-able).
 * Connections that prove the token skip this check at the call site,
 * same rationale as the WS upgrade.
 */
bool ws_config_post_allowed(const char* origin, const char* referer, const char* host_hdr,
                            const char* extra_origins);
