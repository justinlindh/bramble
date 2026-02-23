# CLI Monitor

## Broadcast delivery telemetry stream

`bramble monitor` surfaces `bramble.onBroadcastDelivery` notifications
as live telemetry. Operators should correlate rows by
`(broadcastId, packetId)` and treat updates as timeline events.

### Example output

```text
[23:11:02.514] onBroadcastDelivery broadcastId=bcast_7c912f \
  packetId=pkt_0142 status=delivered hops=2 from=A1B2C3D4 to=FFFFFFFF
[23:11:02.933] onBroadcastDelivery broadcastId=bcast_7c912f \
  packetId=pkt_0142 status=failed hops=3 from=A1B2C3D4 to=FFFFFFFF
```

### Monitoring at scale

- Apply event filters when investigating a single sender or channel.
- Prefer summary counters for long runs.
  Persist raw events to file for post-analysis.
- In dense simulations, sample or bucket by time window
  before alerting.

### CLI example workflow

```bash
# Monitor only broadcast delivery notifications and archive raw output
bramble monitor --events bramble.onBroadcastDelivery | tee broadcast-telemetry.log
```
