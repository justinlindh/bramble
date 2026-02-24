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

## `bramble.onWifiEvent`

WiFi lifecycle transition notifications for monitor/event streams.

Fields include:
- `event`: `connected` | `disconnected` | `ip_changed`
- `mode`: `off` | `sta` | `ap`
- `connected` (bool), `ssid`, `ip`, `rssi`

## `bramble.onGpsEvent`

GPS fix transition notifications.

Fields include:
- `event`: `fix_acquired` | `fix_lost`
- Optional position details on acquire: `lat`, `lon`, `alt_m`, `sats`

## `bramble.onLocationEvent`

Location sharing activity notifications.

Fields include:
- `event`: `sent` | `received`
- `tier`, `timestamp_ms`
- `peer` for received events
- `count` for batched sends
