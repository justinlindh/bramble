/**
 * Bramble Mock Node — WebSocket JSON-RPC 2.0 server
 * Single-file development server. Port 3005.
 *
 * Simulates a realistic 5-node mesh in the Example/Example area of NV.
 * "Our" node (HomeBase) sits in Example. Peers are spread across Example.
 *
 * Implements the same JSON-RPC wire protocol as the real firmware:
 *   Request:      { jsonrpc: "2.0", id: N, method: "bramble.X", params: {...} }
 *   Response:     { jsonrpc: "2.0", id: N, result: {...} }
 *   Notification: { jsonrpc: "2.0", method: "bramble.X", params: {...} }
 */

import { WebSocketServer } from 'ws';
import { handleConnection } from './handler.mjs';

const PORT = Number(process.env.PORT || 3005);

const wss = new WebSocketServer({ port: PORT });

wss.on('listening', () => {
  console.log(`[mock-node] WebSocket server listening on ws://0.0.0.0:${PORT}`);
});

wss.on('connection', handleConnection);
