# RPC Events

## `bramble.onBroadcastDelivery`

Firmware emits `bramble.onBroadcastDelivery` as a JSON-RPC
notification (`jsonrpc: "2.0"`, no `id`) when broadcast
delivery telemetry is available.

### Contract

Fields:

- `broadcastId` (string): matches `bramble.sendBroadcast` response
- `packetId` (string): matches `bramble.sendBroadcast` response
- `from` (string): source node address
- `to` (string): destination (`FFFFFFFF` is common for broadcast)
- `status` (`delivered` | `failed`)
- `hopCount` (integer)
- `deliveredAtMs` (integer, Unix epoch ms)

Semantics:

- One broadcast can emit multiple delivery notifications.
- Consumers should treat events as append-only telemetry,
  not last-write-wins state.
- Correlation key: `(broadcastId, packetId)`.

### JSON-RPC example (firmware to host)

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onBroadcastDelivery",
  "params": {
    "broadcastId": "bcast_7c912f",
    "packetId": "pkt_0142",
    "from": "A1B2C3D4",
    "to": "FFFFFFFF",
    "status": "delivered",
    "hopCount": 2,
    "deliveredAtMs": 1771833505123
  }
}
```

### SDK handling example

```ts
rpc.on("bramble.onBroadcastDelivery", (evt) => {
  const key = `${evt.broadcastId}:${evt.packetId}`;
  deliveryTimeline.push({ key, ...evt });
});
```

## Related API

See also `docs/api/rpc.md` for `bramble.sendBroadcast`
response fields used for telemetry correlation.
