# Bramble RPC API Notes

## WebSocket RPC endpoint

Bramble JSON-RPC over WebSocket uses the `/ws` endpoint.

- Example: `ws://192.168.4.1/ws`
- OpenAPI `/rpc/...` paths are documentation/codegen mappings, not a WebSocket URI.

## `bramble.sendBroadcast` response fields

Current firmware returns snake_case fields:

- `broadcast_id` (string): broadcast correlation ID (8-char uppercase hex)
- `status` (`sent`)
- `broadcast` (boolean)
- `channel` (integer)
- `fragmented` (boolean)
- `fragments_total` (integer, only when fragmented)
- `max_bytes` (integer)
- `actual_bytes` (integer)

## Notification channel: `bramble.onBroadcastDelivery`

Current firmware emits `bramble.onBroadcastDelivery` notifications with:

- `recipient` (string): recipient node address (8-char uppercase hex)
- `broadcast_id` (string): broadcast correlation ID
- `status` (`delivered`)
- `rssi_at_dest` (integer, dBm)
- `relayPath` (optional array in path-sampled mode)
