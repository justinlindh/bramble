# Broadcast Protocol

## Broadcast delivery telemetry modes

Broadcast delivery telemetry is emitted after `sendBroadcast`
and correlated with:

- `broadcastId`: stable logical broadcast identifier
- `packetId`: concrete queued packet identifier

Treat `(broadcastId, packetId)` as the primary correlation key
across firmware, RPC, SDK, CLI, simulator, and web UI surfaces.

### Mode tradeoffs

#### Minimal mode (throughput-first)

Use when RF airtime and battery are constrained
and operators only need coarse success and failure trends.

- Pros: lower event volume, lower memory pressure, less UI churn
- Cons: reduced forensic detail for path and timing analysis

#### Detailed mode (debug-first)

Use during bring-up, regressions, field investigations,
or simulator tuning.

- Pros: richer per-event diagnostics (`status`, `hopCount`,
  `deliveredAtMs`) and better root-cause visibility
- Cons: higher host-side processing and rendering overhead

### Scaling guidance

For dense meshes or high-rate publishers:

1. Prefer minimal mode for routine operation.
2. Enable detailed mode only on targeted nodes and time windows.
3. Bound retained in-memory history by `(broadcastId, packetId)`
   and age.
4. Aggregate in rolling windows for dashboards
   instead of rendering every event.
5. In interactive clients, batch UI updates
   (for example 100 to 250 ms coalescing)
   to avoid frame drops.

## Firmware correlation example

```c
// Pseudocode: after calling sendBroadcast from firmware-side app logic
SendBroadcastResponse resp = sendBroadcast(payload);
cache_insert(resp.broadcastId, resp.packetId, now_ms());

// Later, when a delivery telemetry event arrives:
void on_broadcast_delivery(const BroadcastDeliveryNotification* evt) {
  if (cache_contains(evt->broadcastId, evt->packetId)) {
    update_delivery_stats(evt->status, evt->hopCount, evt->deliveredAtMs);
  }
}
```

## SDK consumer example

```ts
const pending = new Map<string, { createdAt: number }>();

const sent = await client.sendBroadcast({ text: "status beacon" });
pending.set(`${sent.broadcastId}:${sent.packetId}`, { createdAt: Date.now() });

client.on("bramble.onBroadcastDelivery", (evt) => {
  const key = `${evt.broadcastId}:${evt.packetId}`;
  if (!pending.has(key)) return;

  // Append-only telemetry handling
  telemetryStore.append(evt);
});
```
