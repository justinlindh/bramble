#pragma once

/* Start the WebSocket server on port 80.
 * Incoming WS frames on /ws are routed to rpc_dispatch().
 * Registers as RPC notification transport to push to all clients. */
int ws_server_start(void);

/* Stop the WebSocket server. */
void ws_server_stop(void);
