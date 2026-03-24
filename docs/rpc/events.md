# RPC Notification Events

All events are sent as JSON-RPC 2.0 notifications over the WebSocket endpoint (`/ws`):

```json
{ "jsonrpc": "2.0", "method": "<event>", "params": { ... } }
```

When firmware calls `rpc_notify(..., NULL)`, clients may see `"params": null` (or omitted by some transports) and should treat that as an empty payload.

---

## `bramble.onMessage`

**Description**  
Emitted when a chat payload is successfully decoded and stored from mesh RX (single-packet and reassembled fragmented messages).

**Trigger conditions**
- A message packet (or completed fragment set) is received and parsed.
- Message is written to message store first.
- Notification is emitted before ACK/receipt handling (`send_ack(...)` / broadcast delivery receipt queueing).

**Params**

| Field | Type | Description |
|---|---|---|
| `from` | string | Sender address (8-char uppercase hex). |
| `text` | string | Decoded message text. |
| `rssi` | integer | RSSI (dBm) at receiver. |
| `snr` | number | SNR at receiver. |
| `channel` | integer | Channel id (`-1` for public/unset path). |
| `broadcast` | boolean | `true` if classified as broadcast-in. |

**Semantics notes**
- Emitted for both fragmented and non-fragmented receives.
- Correlates to inbound message history; not a delivery confirmation.

**JSON-RPC example**

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onMessage",
  "params": {
    "from": "A1B2C3D4",
    "text": "Hello from mesh",
    "rssi": -94,
    "snr": 7.5,
    "channel": -1,
    "broadcast": true
  }
}
```

---

## `bramble.onAck`

**Description**  
Emitted for outgoing unicast delivery state transitions: delivered ACKs and failures.

**Trigger conditions**
- **Delivered**: receipt/ACK processing updates message status to delivered.
- **Failed**: pending ACK is failed due to retry timeout or route-error fast-fail (`RERR`).

**Params**

| Field | Type | Present when | Description |
|---|---|---|---|
| `packet_id` | string | always | Packet id (8-char uppercase hex) matching send response. |
| `status` | string | always | `"delivered"` or `"failed"`. |
| `from` | string | delivered | Acknowledging node address. |
| `rssi_at_dest` | integer | delivered | Destination-reported RSSI. |
| `relayPath` | array<object> | delivered | Ordered relay hops; each hop has `addr` (string) and `rssi` (integer). |
| `reason` | string | failed (optional) | Failure reason (e.g. `"route_broken"`). |

**Semantics notes**
- Failures may be emitted without `from`/`relayPath`.
- Use `packet_id` as the correlation key.

**JSON-RPC example (delivered)**

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onAck",
  "params": {
    "packet_id": "0F3A21CC",
    "status": "delivered",
    "from": "A1B2C3D4",
    "rssi_at_dest": -89,
    "relayPath": [
      { "addr": "11223344", "rssi": 0 },
      { "addr": "A1B2C3D4", "rssi": -89 }
    ]
  }
}
```

**JSON-RPC example (failed)**

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onAck",
  "params": {
    "packet_id": "0F3A21CC",
    "status": "failed",
    "reason": "route_broken"
  }
}
```

---

## `bramble.onLocationEvent`

**Description**  
Location-sharing activity event for local sends and peer location receives.

**Trigger conditions**
- `event: "sent"`: periodic location fan-out sends one or more location payloads to peers.
- `event: "received"`: location packet received, parsed, cached, and persisted.

**Params**

| Field | Type | Present when | Description |
|---|---|---|---|
| `event` | string | always | `"sent"` or `"received"`. |
| `tier` | string | always | Privacy tier string from firmware policy. |
| `timestamp_ms` | integer | always | Milliseconds since boot. |
| `peer` | string | received | Sender address (8-char uppercase hex). |
| `rssi` | integer | received | Receive RSSI (dBm). |
| `snr` | number | received | Receive SNR. |
| `count` | integer | sent | Number of peers included in send batch. |

**Semantics notes**
- `bramble.onPeerLocation` and `bramble.onLocationEvent(event="received")` are emitted together on peer location ingest.

**JSON-RPC example (received)**

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onLocationEvent",
  "params": {
    "event": "received",
    "peer": "A1B2C3D4",
    "tier": "coarse",
    "timestamp_ms": 123456,
    "rssi": -87,
    "snr": 6
  }
}
```

---

## `bramble.onPeerLocation`

**Description**  
Emitted when a peer location packet has been processed and persisted.

**Trigger conditions**
- Location packet is decoded for tier.
- Cache update + NVS persistence complete.
- Notification emitted immediately before `bramble.onLocationEvent(event="received")`.

**Params**
- No payload (`params: null`).

**Semantics notes**
- Treat this as an invalidation signal; call `bramble.getPeerLocations` for full state.

**JSON-RPC example**

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onPeerLocation",
  "params": null
}
```

---

## `bramble.onIdentityChange`

**Description**  
Emitted when node identity is regenerated due to detected address collision.

**Trigger conditions**
- Incoming authenticated beacon indicates same address but different pubkey hash.
- Firmware regenerates keypair/identity and persists it.
- Event emitted, then collision-path processing returns early.

**Params**

| Field | Type | Description |
|---|---|---|
| `new_address` | string | New node address (8-char uppercase hex). |
| `reason` | string | Currently `"address_collision"`. |

**Semantics notes**
- Clients should immediately refresh cached local identity/address references.

**JSON-RPC example**

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

## `bramble.onNeighborChange`

**Description**  
Emitted whenever beacon handling mutates/refreshes neighbor table data.

**Trigger conditions**
- Valid non-self beacon processed and neighbor state updated.
- Emitted after neighbor update/state bookkeeping within beacon handler.

**Params**
- No payload (`params: null`).

**Semantics notes**
- No delta is included; call `bramble.getNeighbors` for current table.

**JSON-RPC example**

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onNeighborChange",
  "params": null
}
```

---

## `bramble.onProbeComplete`

**Description**  
Terminal event for an active probe sweep window.

**Trigger conditions**
- Probe collection is active and collection window elapses.
- Summary is built from accumulated responder state.
- Event emitted once; probe collection is then marked complete.

**Params**

| Field | Type | Description |
|---|---|---|
| `probe_id` | string | Active probe id (8-char uppercase hex). |
| `unique_count` | integer | Number of unique responders seen. |
| `duration_ms` | integer | Elapsed probe collection duration. |
| `rounds_total` | integer | Total probe rounds configured/sent. |
| `responders` | array<object> | Aggregated responder records. |

Responder object fields:

| Field | Type | Description |
|---|---|---|
| `address` | string | Responder address (8-char uppercase hex). |
| `hops` | integer | Best/observed hops. |
| `rssi` | integer | Best/observed RSSI. |
| `snr` | number | Best/observed SNR. |
| `latency_ms` | integer | Last measured latency. |
| `seen_rounds` | integer | Number of probe rounds with responses. |

**Semantics notes**
- Exactly one complete event per active probe id.

**JSON-RPC example**

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
      { "address": "DEADBEEF", "hops": 1, "rssi": -78, "snr": 9, "latency_ms": 145, "seen_rounds": 3 },
      { "address": "CAFEF00D", "hops": 2, "rssi": -95, "snr": 4.5, "latency_ms": 312, "seen_rounds": 2 }
    ]
  }
}
```

---

## `bramble.onProbeResult`

**Description**  
Per-response event during an active probe sweep.

**Trigger conditions**
- Probe ACK received and accepted while probe collection is active.
- Per-responder aggregate state updated.
- Event emitted for that response.

**Params**

| Field | Type | Description |
|---|---|---|
| `address` | string | Responder address (8-char uppercase hex). |
| `hops` | integer | Hop count in this response. |
| `rssi` | integer | RSSI for this responder/sample. |
| `snr` | number | SNR for this responder/sample. |
| `latency_ms` | integer | Probe RTT in milliseconds. |
| `probe_round` | integer | Probe round number (1-based). |
| `probe_id` | string | Active probe id (8-char uppercase hex). |

**Semantics notes**
- Multiple events can occur for one responder across rounds.
- `bramble.onProbeComplete` closes the stream for that probe.

**JSON-RPC example**

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

---

## `bramble.onBroadcastDelivery`

**Description**  
Broadcast delivery telemetry event for recipient confirmations.

**Trigger conditions**
- A delivery receipt for a previously sent broadcast is processed.
- Telemetry record is persisted (`record_broadcast_delivery_event(...)`).
- Notification is emitted.

**Params**

| Field | Type | Description |
|---|---|---|
| `recipient` | string | Recipient node address (8-char uppercase hex). |
| `broadcast_id` | string | Broadcast id (8-char uppercase hex). |
| `status` | string | Currently always `"delivered"`. |
| `rssi_at_dest` | integer | RSSI measured at recipient. |
| `relayPath` | array<object> | Optional relay hops; present in path-sampled telemetry mode. |

Relay path object fields: `addr` (string).

**Semantics notes**
- One broadcast can emit multiple events (one per recipient/report).
- Correlate on `broadcast_id` + `recipient`.

**JSON-RPC example**

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

## `bramble.onGpsEvent`

**Description**  
GPS connectivity state change event.

**Trigger conditions**
- GPS fix acquired or lost (state transition).
- Polled periodically by the connectivity event loop.

**Params**

| Field | Type | Present when | Description |
|---|---|---|---|
| `event` | string | always | `"fix_acquired"` or `"fix_lost"`. |
| `valid` | boolean | fix_acquired | Whether the position is valid. |
| `lat` | number | fix_acquired | Latitude in degrees. |
| `lon` | number | fix_acquired | Longitude in degrees. |
| `alt_m` | number | fix_acquired | Altitude in meters. |
| `accuracy_m` | number | fix_acquired | Position accuracy in meters. |

**Semantics notes**
- Only emitted on state transitions (fix gained or lost), not on every GPS update.
- `fix_lost` events have no position fields.

**JSON-RPC example (fix acquired)**

```json
{
  "jsonrpc": "2.0",
  "method": "bramble.onGpsEvent",
  "params": {
    "event": "fix_acquired",
    "valid": true,
    "lat": 36.0544,
    "lon": -115.0523,
    "alt_m": 570,
    "accuracy_m": 3.2
  }
}
```

**JSON-RPC example (fix lost)**

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

## `bramble.onWifiEvent`

**Description**  
Wi-Fi connectivity state change event.

**Trigger conditions**
- Wi-Fi connection or disconnection (state transition).
- Wi-Fi mode change (off/station/AP).
- Polled periodically by the connectivity event loop.

**Params**

| Field | Type | Description |
|---|---|---|
| `event` | string | Event type (`"connected"`, `"disconnected"`, `"mode_changed"`). |
| `mode` | string | Wi-Fi mode: `"off"`, `"sta"`, or `"ap"`. |
| `connected` | boolean | Whether an IP address is assigned. |
| `ssid` | string | Connected SSID (omitted if empty). |
| `ip` | string | IP address (empty string if not assigned). |
| `rssi` | integer | Wi-Fi RSSI (dBm). |

**Semantics notes**
- Only emitted on state transitions, not on every poll cycle.
- `ssid` is omitted when no SSID is available.

**JSON-RPC example (connected)**

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

---

## `bramble.onTrafficEvent`

**Description**  
Real-time packet telemetry event for traffic debug stream.

**Trigger conditions**
- Traffic debug recorder emits TX/RX event and callback builds JSON payload.
- Event pushed via `rpc_notify` to WebSocket clients.

**Params**

| Field | Type | Description |
|---|---|---|
| `seq` | integer | Monotonic event sequence number. |
| `timestamp_ms` | integer | Milliseconds since boot. |
| `pkt_type` | integer | Raw packet type id. |
| `category` | string | Packet category (`beacon`, `timesync`, `routing`, `ack`, `chat`, `maintenance`, `other`, `unknown`). |
| `airtime_tier` | string | Airtime tier (`none`, `normal`, `critical`, `broadcast`, `unknown`). |
| `packet_len` | integer | Packet length in bytes. |
| `rssi` | integer | RX RSSI (or 0 for TX events). |
| `is_tx` | boolean | `true` for TX, `false` for RX. |

**Semantics notes**
- Stream behavior depends on traffic debug configuration and sampling.

**JSON-RPC example**

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