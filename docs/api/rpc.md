# Bramble RPC API Notes

## `bramble.sendBroadcast` response telemetry fields

`bramble.sendBroadcast` returns `SendBroadcastResponse` with correlation fields for delivery telemetry:

- `broadcastId` (string): stable broadcast identifier for the logical broadcast send.
- `packetId` (string): packet identifier for the queued outbound broadcast packet.

Clients should persist both values and use them to correlate subsequent broadcast delivery notifications.

## Notification channel: `bramble.onBroadcastDelivery`

Firmware emits `bramble.onBroadcastDelivery` JSON-RPC notifications (`jsonrpc: "2.0"`, no `id`) when broadcast delivery telemetry is available.

Payload schema: `BroadcastDeliveryNotification`

- `broadcastId` (string): matches `SendBroadcastResponse.broadcastId`
- `packetId` (string): matches `SendBroadcastResponse.packetId`
- `from` (string): source node address (8-char uppercase hex)
- `to` (string): destination address (typically `FFFFFFFF` for broadcast)
- `status` (`delivered` | `failed`): delivery outcome for this event
- `hopCount` (integer): number of hops observed for this delivery event
- `deliveredAtMs` (integer): event timestamp (Unix ms)

### Semantics

- A single broadcast send may result in multiple `bramble.onBroadcastDelivery` notifications.
- Correlation key is the tuple `(broadcastId, packetId)`.
- Consumers should treat notifications as append-only telemetry events rather than overwriting prior state.
