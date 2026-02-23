# Web App Chat

## Broadcast delivery telemetry UX

The chat view uses broadcast delivery telemetry
to show confidence and outcome for recently sent
broadcast messages.

### UX behavior

- Compact view: shows a lightweight indicator
  for recent delivery activity.
- Expanded details: shows append-only timeline entries
  keyed by `(broadcastId, packetId)`.
- Failed outcomes remain visible long enough
  for operator review.

### Scaling and operator guidance

In active channels:

- Coalesce repaint and update cycles
  to avoid jitter during telemetry bursts.
- Keep expanded timelines bounded by count
  and time window.
- Prefer summary counters in list views;
  expand details on demand.

### Web example

```ts
store.subscribeToRpc("bramble.onBroadcastDelivery", (evt) => {
  const key = `${evt.broadcastId}:${evt.packetId}`;
  store.broadcastTelemetry.append(key, evt);

  // List row signal for compact mode
  store.chat.bumpDeliveryIndicator(evt.broadcastId, evt.status);
});
```
