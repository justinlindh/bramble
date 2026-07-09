# Simulator Events

## Broadcast delivery telemetry events

> **Status: parity spec, not as-built reference.** This documents the
> intended simulator-side event shape. Note the field names here are
> camelCase (`broadcastId`, `hopCount`, `deliveredAtMs`) while the
> firmware wire payload is snake_case (`broadcast_id`, `rssi_at_dest`;
> see `docs/rpc/events.md`). Verify against current gosim output before
> relying on exact fields.

The simulator should mirror firmware RPC semantics
for broadcast delivery telemetry so client behavior
can be validated before device deployment.

### Event parity expectations

Simulator `onBroadcastDelivery` payloads should preserve:

- `broadcastId`, `packetId` correlation fields
- outcome `status`
- path signal (`hopCount`)
- wall-clock style delivery timestamp (`deliveredAtMs`)

### Simulator scaling scenarios

Use telemetry to validate behavior under load:

1. Low-rate baseline: verify stable delivered and failed ratios.
2. Burst traffic: verify clients remain responsive
   while event rate spikes.
3. Dense mesh: verify aggregation and downsampling choices
   in dashboards.

### Simulator event example

```json
{
  "type": "onBroadcastDelivery",
  "broadcastId": "bcast_7c912f",
  "packetId": "pkt_0142",
  "status": "delivered",
  "hopCount": 3,
  "deliveredAtMs": 1771833505123
}
```
