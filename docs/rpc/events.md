# RPC Events

## `bramble.onBroadcastDelivery`

Firmware emits `bramble.onBroadcastDelivery` as a JSON-RPC
notification (`jsonrpc: "2.0"`, no `id`) when broadcast
delivery telemetry is available.

### Contract (current firmware)

Fields:

- `recipient` (string): recipient node address
- `broadcast_id` (string): matches `bramble.sendBroadcast` response
- `status` (`delivered`)
- `rssi_at_dest` (integer)
- `relayPath` (optional array): sampled relay path hops (`{ addr }`)

Semantics:

- One broadcast can emit multiple delivery notifications.
- Consumers should treat events as append-only telemetry.
- Correlation key: `broadcast_id` + `recipient`.

### JSON-RPC example (firmware to host)

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onBroadcastDelivery",
  "params": {
    "recipient": "A1B2C3D4",
    "broadcast_id": "7C912F42",
    "status": "delivered",
    "rssi_at_dest": -97,
    "relayPath": [{ "addr": "11223344" }]
  }
}
```
