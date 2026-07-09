# CLI Monitor

## Broadcast delivery telemetry stream

`bramble monitor` surfaces `bramble.onBroadcastDelivery` notifications
as live telemetry. Current firmware payload fields are snake_case.

Correlate events by `(broadcast_id, recipient)` and treat updates as
append-only timeline events.

### Example output

```text
[23:11:02.514] onBroadcastDelivery broadcast_id=7C912F42 \
  recipient=A1B2C3D4 status=delivered rssi_at_dest=-97
[23:11:02.933] onBroadcastDelivery broadcast_id=7C912F42 \
  recipient=3E7A10BC status=delivered rssi_at_dest=-103
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
bramble monitor --events broadcast-delivery | tee broadcast-telemetry.log
```

`--events` takes short event names (`message`, `ack`, `neighbor`,
`broadcast-delivery`), not RPC method names. A separate `--topic` flag
filters topic streams (`wifi,gps,mesh,location,traffic`).
