#pragma once

#include <stdbool.h>

void ws_server_load_token(void);
const char *ws_server_get_token(void);

/* Start the WebSocket server on port 80.
 * Incoming WS frames on /ws are routed to rpc_dispatch().
 * Registers as RPC notification transport to push to all clients.
 * Idempotent: safe to call multiple times. */
int ws_server_start(void);

/* Stop the WebSocket server. */
void ws_server_stop(void);

/* Check if WebSocket server is running. */
bool ws_server_is_running(void);
