# RPC Notification Events

All notifications are emitted as JSON-RPC 2.0 notifications (no `id` field) over
the WebSocket connection at `/ws`.

```json
{ "jsonrpc": "2.0", "method": "<method>", "params": { ... } }
```

When `params` is `null` the firmware calls `rpc_notify` with a NULL payload;
clients should treat a missing or null `params` as an empty object.

---

## `bramble.onMessage`

Emitted when a unicast, channel, or broadcast message is received from the mesh.

### Payload fields

| Field | Type | Description |
|---|---|---|
| `from` | string | Sender address as 8-char uppercase hex. |
| `text` | string | Message text content. |
| `rssi` | integer | Received signal strength in dBm. |
| `snr` | number | Signal-to-noise ratio in dB. |
| `channel` | integer | Channel index, or `-1` for the public channel. |
| `broadcast` | boolean | `true` when received as a network broadcast. |

### Example

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onMessage",
  "params": {
    "from": "A1B2C3D4",
    "text": "Hello from the mesh",
    "rssi": -94,
    "snr": 7.5,
    "channel": -1,
    "broadcast": false
  }
}
```

---

## `bramble.onAck`

Emitted when a delivery acknowledgment is received for a previously sent message,
or when delivery fails (e.g. a RERR route-error is received).

### Payload fields

| Field | Type | Description |
|---|---|---|
| `packet_id` | string | Packet ID matching `packetId` from `bramble.sendMessage`. |
| `from` | string | Acknowledging node address as 8-char uppercase hex (success only). |
| `status` | string | `"delivered"` or `"failed"`. |
| `rssi_at_dest` | integer | RSSI measured at the destination (success only). |
| `relayPath` | array | Relay hops `{ addr, rssi }` from sender to destination (success only). |
| `reason` | string | Failure reason string when `status = "failed"`. |

### Semantics

- `status: "failed"` is emitted when a RERR is received for a pending packet.
- Correlation key: `packet_id`.

### Example — delivered

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onAck",
  "params": {
    "from": "A1B2C3D4",
    "packet_id": "0F3A21CC",
    "status": "delivered",
    "rssi_at_dest": -89,
    "relayPath": [
      { "addr": "11223344", "rssi": -80 },
      { "addr": "A1B2C3D4", "rssi": -89 }
    ]
  }
}
```

### Example — failed

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onAck",
  "params": {
    "packet_id": "0F3A21CC",
    "status": "failed",
    "reason": "no_route"
  }
}
```

---

## `bramble.onNeighborChange`

Emitted whenever the neighbor table is updated (new neighbor seen, or existing
neighbor refreshed via beacon). The notification carries no payload.

Clients should call `bramble.getNeighbors` to retrieve the updated table.

### Example

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onNeighborChange",
  "params": null
}
```

---

## `bramble.onProbeResult`

Emitted for each probe response received during an active `bramble.sendProbe`
sweep. Multiple results may arrive before the sweep ends with
`bramble.onProbeComplete`.

### Payload fields

| Field | Type | Description |
|---|---|---|
| `probe_id` | string | Probe identifier matching the `sendProbe` response. |
| `address` | string | Responding node address as 8-char uppercase hex. |
| `hops` | integer | Hop count to the responding node. |
| `rssi` | integer | Best RSSI observed for this responder across rounds. |
| `snr` | number | Best SNR observed for this responder. |
| `latency_ms` | integer | Round-trip latency in milliseconds. |
| `probe_round` | integer | Probe round number (1-based) in which this response arrived. |

### Example

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onProbeResult",
  "params": {
    "probe_id": "0000CAFE",
    "address": "DEADBEEF",
    "hops": 2,
    "rssi": -95,
    "snr": 5.0,
    "latency_ms": 312,
    "probe_round": 1
  }
}
```

---

## `bramble.onProbeComplete`

Emitted once when the probe collection window closes. Summarises all results
gathered during the sweep. No further `bramble.onProbeResult` events will follow
for this probe.

### Payload fields

| Field | Type | Description |
|---|---|---|
| `probe_id` | string | Probe identifier matching the `sendProbe` response. |
| `unique_count` | integer | Number of unique nodes that responded. |
| `duration_ms` | integer | Total probe duration in milliseconds. |
| `rounds_total` | integer | Number of probe rounds sent. |
| `responders` | array | Summary of all responding nodes (see below). |

Each `responders` entry:

| Field | Type | Description |
|---|---|---|
| `address` | string | Node address as 8-char uppercase hex. |
| `hops` | integer | Hop count. |
| `rssi` | integer | Best RSSI across all rounds. |
| `snr` | number | Best SNR across all rounds. |
| `latency_ms` | integer | Most recent round-trip latency. |
| `seen_rounds` | integer | Number of rounds in which this node responded. |

### Example

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onProbeComplete",
  "params": {
    "probe_id": "0000CAFE",
    "unique_count": 2,
    "duration_ms": 4800,
    "rounds_total": 3,
    "responders": [
      { "address": "DEADBEEF", "hops": 1, "rssi": -78, "snr": 9.0, "latency_ms": 145, "seen_rounds": 3 },
      { "address": "CAFEF00D", "hops": 2, "rssi": -95, "snr": 4.5, "latency_ms": 312, "seen_rounds": 2 }
    ]
  }
}
```

---

## `bramble.onTrafficEvent`

Real-time traffic event emitted when traffic debug is enabled
(see `bramble.setTrafficDebug`). One notification is emitted per TX or RX packet.

### Payload fields

| Field | Type | Description |
|---|---|---|
| `seq` | integer | Monotonic sequence number. |
| `timestamp_ms` | integer | Milliseconds since boot. |
| `pkt_type` | integer | Raw packet type ID from firmware `packet.h`. |
| `category` | string | `beacon` \| `timesync` \| `routing` \| `ack` \| `chat` \| `maintenance` \| `other` \| `unknown` |
| `airtime_tier` | string | `none` \| `normal` \| `critical` \| `broadcast` \| `unknown` |
| `packet_len` | integer | On-air payload length in bytes. |
| `rssi` | integer | Received signal strength in dBm (0 for TX events). |
| `is_tx` | boolean | `true` for TX events, `false` for RX. |

### Example

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onTrafficEvent",
  "params": {
    "seq": 42,
    "timestamp_ms": 123456,
    "pkt_type": 1,
    "category": "beacon",
    "airtime_tier": "normal",
    "packet_len": 48,
    "rssi": -91,
    "is_tx": false
  }
}
```

---

## `bramble.onLocationEvent`

Emitted when the node sends or receives a location packet.

### Payload fields

| Field | Type | Present when |
|---|---|---|
| `event` | string | Always — `"sent"` or `"received"`. |
| `peer` | string | `received` — sender address as 8-char uppercase hex. |
| `tier` | integer | Always — sharing tier (0=full, 1=coarse, 2=presence, 3=off). |
| `timestamp_ms` | integer | Always — milliseconds since boot. |
| `rssi` | integer | `received` — signal strength in dBm. |
| `snr` | number | `received` — signal-to-noise ratio in dB. |
| `count` | integer | `sent` — number of peers included in the send batch. |

### Example — received

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onLocationEvent",
  "params": {
    "event": "received",
    "peer": "A1B2C3D4",
    "tier": 1,
    "timestamp_ms": 98765,
    "rssi": -87,
    "snr": 6.0
  }
}
```

### Example — sent

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onLocationEvent",
  "params": {
    "event": "sent",
    "tier": 1,
    "timestamp_ms": 102000,
    "count": 3
  }
}
```

---

## `bramble.onPeerLocation`

Emitted immediately after a peer location packet is received and persisted to
NVS. Carries no payload — clients should call `bramble.getPeerLocations` to
retrieve the full updated set.

### Example

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onPeerLocation",
  "params": null
}
```

---

## `bramble.onIdentityChange`

Emitted when the node detects an address collision with another peer and
regenerates its identity. Clients must update any stored reference to the node
address immediately.

### Payload fields

| Field | Type | Description |
|---|---|---|
| `new_address` | string | New node address as 8-char uppercase hex. |
| `reason` | string | Reason for the change — currently always `"address_collision"`. |

### Example

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onIdentityChange",
  "params": {
    "new_address": "7F3A1B2C",
    "reason": "address_collision"
  }
}
```

---

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

Firmware emits `bramble.onBroadcastDelivery` when broadcast delivery telemetry
is available (controlled by `bramble.setBroadcastTelemetryMode`). One broadcast
may produce multiple delivery notifications — one per recipient that reports back.

### Payload fields

| Field | Type | Description |
|---|---|---|
| `recipient` | string | Recipient node address as 8-char uppercase hex. |
| `broadcast_id` | string | Matches `broadcast_id` from `bramble.sendBroadcast` response. |
| `status` | string | Always `"delivered"`. |
| `rssi_at_dest` | integer | RSSI measured at the recipient, in dBm. |
| `relayPath` | array | Relay hops `{ addr }` — present only in `path_sampled` telemetry mode. |

### Semantics

- One broadcast can emit multiple delivery notifications.
- Consumers should treat events as append-only telemetry.
- Correlation key: `broadcast_id` + `recipient`.

### Example

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


---

## `bramble.onTrafficEvent`

Emitted when traffic debug capture is enabled and a TX or RX packet event is
recorded. Not emitted when traffic debug is disabled. Use
`bramble.getTrafficEvents` for replay/backfill.

### Payload fields

| Field | Type | Description |
|---|---|---|
| `seq` | integer | Monotonic event sequence number. |
| `timestamp_ms` | integer | Event timestamp (ms since boot). |
| `pkt_type` | integer | Packet type id. |
| `category` | string | `"beacon"` \| `"timesync"` \| `"routing"` \| `"ack"` \| `"chat"` \| `"maintenance"` \| `"other"` \| `"unknown"` |
| `airtime_tier` | string | `"none"` \| `"normal"` \| `"critical"` \| `"broadcast"` \| `"unknown"` |
| `packet_len` | integer | Packet length in bytes. |
| `rssi` | integer | RSSI for RX events, 0 for TX events. |
| `is_tx` | boolean | `true` for TX events. |

### Example

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

---

## `bramble.onGpsEvent`

GPS fix transition notification. Emitted on fix acquire (with position fields)
and on fix loss (without position fields). Only emitted on boards with GPS
hardware capability.

### Payload fields

| Field | Type | Present when |
|---|---|---|
| `event` | string | Always — `"fix_acquired"` or `"fix_lost"`. |
| `valid` | boolean | `fix_acquired` — always `true`. |
| `lat` | number | `fix_acquired` — latitude in decimal degrees. |
| `lon` | number | `fix_acquired` — longitude in decimal degrees. |
| `alt_m` | integer | `fix_acquired` — altitude in meters. |
| `accuracy_m` | integer | `fix_acquired` — horizontal accuracy in meters. |

### Example — fix acquired

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onGpsEvent",
  "params": {
    "event": "fix_acquired",
    "valid": true,
    "lat": 37.7749,
    "lon": -122.4194,
    "alt_m": 12,
    "accuracy_m": 5
  }
}
```

### Example — fix lost

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onGpsEvent",
  "params": {
    "event": "fix_lost"
  }
}
```

---

## `bramble.onLocationEvent`

Location sharing activity notification. Emitted when local periodic shares are
sent or peer location packets are received.

### Payload fields

| Field | Type | Description |
|---|---|---|
| `event` | string | `"sent"` \| `"received"` |
| `tier` | integer | Privacy tier level. |
| `timestamp_ms` | integer | Event timestamp (ms since boot). |
| `peer` | string | Peer address as hex (only for `received`). |
| `rssi` | integer | RSSI in dBm (only for `received`). |
| `snr` | integer | SNR in dB (only for `received`). |
| `count` | integer | Batch recipient count (only for `sent`). |

### Example

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

---

## `bramble.onWifiEvent`

WiFi lifecycle transition notification. Emitted on connect, disconnect, and IP
address change.

### Payload fields

| Field | Type | Description |
|---|---|---|
| `event` | string | `"connected"` \| `"disconnected"` \| `"ip_changed"` |
| `mode` | string | `"off"` \| `"sta"` \| `"ap"` |
| `connected` | boolean | `true` when an IP address is assigned. |
| `ssid` | string | Current SSID (omitted when empty). |
| `ip` | string | Current IPv4 address (empty string when disconnected). |
| `rssi` | integer | Station RSSI in dBm (0 in AP mode or when disconnected). |

### Example

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onWifiEvent",
  "params": {
    "event": "connected",
    "mode": "sta",
    "connected": true,
    "ssid": "MyNetwork",
    "ip": "192.168.1.42",
    "rssi": -65
  }
}
```
