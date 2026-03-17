# RPC Events

## `bramble.onMessage`

Firmware emits `bramble.onMessage` as a JSON-RPC notification when a chat or
broadcast payload is successfully decoded and stored.

### Contract (current firmware)

Fields:

- `from` (string): sender node address (8-char hex)
- `text` (string): decoded message text
- `rssi` (integer): RSSI of received packet (dBm)
- `snr` (integer): SNR of received packet
- `channel` (integer): channel index, or `-1` for public broadcast path
- `broadcast` (boolean): true when message direction is broadcast-in

Semantics:

- Emitted for both fragmented and single-packet messages after reassembly.
- Emitted before receipt/ACK handling is completed.

### JSON-RPC example (firmware to host)

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onMessage",
  "params": {
    "from": "A1B2C3D4",
    "text": "hello",
    "rssi": -98,
    "snr": 7,
    "channel": -1,
    "broadcast": true
  }
}
```

## `bramble.onAck`

Firmware emits `bramble.onAck` when a delivery receipt is processed for an
outgoing unicast message and message status transitions to delivered.

### Contract (current firmware)

Fields:

- `from` (string): destination peer that acknowledged
- `packet_id` (string): packet id returned by `bramble.sendMessage`
- `status` (`delivered`)
- `rssi_at_dest` (integer): RSSI reported by destination
- `relayPath` (array): hop list (`{ addr, rssi }`)

Semantics:

- Relay path is normalized sender→...→destination for UI display.
- Emitted only when the message store entry is found and updated.

### JSON-RPC example (firmware to host)

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onAck",
  "params": {
    "from": "11223344",
    "packet_id": "7C912F42",
    "status": "delivered",
    "rssi_at_dest": -94,
    "relayPath": [{ "addr": "A1B2C3D4", "rssi": 0 }]
  }
}
```

## `bramble.onNeighborChange`

Firmware emits `bramble.onNeighborChange` whenever beacon handling updates the
neighbor table.

### Contract (current firmware)

Fields:

- No payload (`params` omitted)

Semantics:

- Triggered on every neighbor table mutation during beacon processing.
- Consumers should call `bramble.getNeighbors` to fetch the updated snapshot.

### JSON-RPC example (firmware to host)

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onNeighborChange"
}
```

## `bramble.onProbeResult`

Firmware emits `bramble.onProbeResult` for each probe ACK received while a
probe sweep is active.

### Contract (current firmware)

Fields:

- `address` (string): responder node address
- `hops` (integer): measured hop count
- `rssi` (integer): best RSSI seen for responder
- `snr` (integer): best SNR seen for responder
- `latency_ms` (integer): latency from probe send to response
- `probe_round` (integer): probe sweep round number
- `probe_id` (string): active probe id (hex)

Semantics:

- Multiple results may arrive per responder across rounds.
- Firmware coalesces per-responder state internally but still emits per-ACK events.

### JSON-RPC example (firmware to host)

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onProbeResult",
  "params": {
    "address": "CAFEBABE",
    "hops": 2,
    "rssi": -102,
    "snr": 4,
    "latency_ms": 318,
    "probe_round": 2,
    "probe_id": "00AA11BB"
  }
}
```

## `bramble.onProbeComplete`

Firmware emits `bramble.onProbeComplete` once the probe window closes.

### Contract (current firmware)

Fields:

- `probe_id` (string): completed probe id (hex)
- `unique_count` (integer): unique responders seen
- `duration_ms` (integer): elapsed probe duration
- `rounds_total` (integer): configured round count
- `responders` (array): responder summary entries:
  - `address` (string)
  - `hops` (integer)
  - `rssi` (integer)
  - `snr` (integer)
  - `latency_ms` (integer)
  - `seen_rounds` (integer)

Semantics:

- Emitted once per probe id after collection timeout expires.
- No additional `onProbeResult` events should follow for the same probe.

### JSON-RPC example (firmware to host)

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onProbeComplete",
  "params": {
    "probe_id": "00AA11BB",
    "unique_count": 3,
    "duration_ms": 5008,
    "rounds_total": 3,
    "responders": [{ "address": "CAFEBABE", "hops": 2, "rssi": -102, "snr": 4, "latency_ms": 318, "seen_rounds": 2 }]
  }
}
```

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

## `bramble.onTrafficEvent`

Firmware emits `bramble.onTrafficEvent` when traffic debug capture is enabled
and a TX or RX packet event is recorded.

### Contract (current firmware)

Fields:

- `seq` (integer): monotonic event sequence
- `timestamp_ms` (integer): event timestamp (ms since boot)
- `pkt_type` (integer): packet type id
- `category` (string): `beacon|timesync|routing|ack|chat|maintenance|other|unknown`
- `airtime_tier` (string): `none|normal|critical|broadcast|unknown`
- `packet_len` (integer): packet length in bytes
- `rssi` (integer): RSSI for RX, 0 for TX
- `is_tx` (boolean): true for TX events

Semantics:

- Not emitted when traffic debug is disabled.
- Use `bramble.getTrafficEvents` for replay/backfill.

### JSON-RPC example (firmware to host)

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onTrafficEvent",
  "params": {
    "seq": 42,
    "timestamp_ms": 912345,
    "pkt_type": 3,
    "category": "ack",
    "airtime_tier": "normal",
    "packet_len": 29,
    "rssi": -95,
    "is_tx": false
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

Location sharing activity notifications emitted when local periodic shares are
sent or peer location packets are received.

### Contract (current firmware)

Fields include:
- `event`: `sent` | `received`
- `tier` (integer)
- `timestamp_ms` (integer)
- `peer` (string, only for `received`)
- `rssi` and `snr` (integers, received path)
- `count` (integer, sent batch recipient count)

### JSON-RPC example (firmware to host)

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onLocationEvent",
  "params": {
    "event": "received",
    "peer": "A1B2C3D4",
    "tier": 1,
    "timestamp_ms": 1234567,
    "rssi": -101,
    "snr": 6
  }
}
```
