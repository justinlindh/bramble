#pragma once

#include <stdbool.h>

void ws_server_load_token(void);
const char* ws_server_get_token(void);

/* True only when auth was EXPLICITLY disabled (empty token set via an
 * authenticated bramble.setAuthToken call). A missing or unreadable token
 * is NOT open access; that state fails closed. Shared by the BLE transport
 * so both network transports apply the same policy. */
bool ws_server_auth_disabled(void);

/* (Re)load the extra allowed WS origins from NVS, and read the cached
 * comma-separated list. Same-origin connections are always allowed; this
 * list is for hosted webapp origins and the like (see main/ws_origin.h). */
void ws_server_load_origins(void);
const char* ws_server_get_extra_origins(void);

/* Start the WebSocket server on port 80.
 * Incoming WS frames on /ws are routed to rpc_dispatch().
 * Registers as RPC notification transport to push to all clients.
 * Idempotent: safe to call multiple times. */
int ws_server_start(void);

/* Stop the WebSocket server. */
void ws_server_stop(void);

/* Check if WebSocket server is running. */
bool ws_server_is_running(void);
