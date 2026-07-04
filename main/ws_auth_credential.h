#ifndef BRAMBLE_WS_AUTH_CREDENTIAL_H
#define BRAMBLE_WS_AUTH_CREDENTIAL_H

#include <stddef.h>
#include <stdbool.h>

/*
 * Extract the presented WebSocket auth token from browser-safe handshake
 * headers ONLY. The legacy ?token= query-string path is intentionally absent
 * from this interface: URLs leak via logs, proxies, and browser history
 * (NEW-SEC-6). Two header carriers are supported:
 *   1. "Authorization: Bearer <token>"  (non-browser clients)
 *   2. "Sec-WebSocket-Protocol: bramble.v1.auth.<token>, bramble.v1"
 *      The browser WebSocket() constructor cannot set headers but CAN offer
 *      subprotocols; the server validates the token from the bramble.v1.auth.
 *      entry and echoes back the non-secret "bramble.v1" (see ws_server.c).
 *      This keeps the token out of the URL, browser history, referer, and the
 *      handshake response. It still rides a REQUEST header, so a proxy or
 *      access log that records request headers can capture it; this is strictly
 *      better than ?token= but is not "never logged anywhere."
 *
 * authorization / subprotocols: header values, or NULL / "" if absent.
 * out / out_len: receives the NUL-terminated token on success.
 * Returns true and fills out iff a non-empty token is found that fits out.
 */
bool ws_auth_extract_token(const char* authorization, const char* subprotocols, char* out,
                           size_t out_len);

#endif /* BRAMBLE_WS_AUTH_CREDENTIAL_H */
