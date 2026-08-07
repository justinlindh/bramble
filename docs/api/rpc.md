# Bramble RPC API Notes

## WebSocket RPC endpoint

Bramble JSON-RPC over WebSocket uses the `/ws` endpoint.

- Example: `ws://192.168.4.1/ws`
- OpenAPI `/rpc/...` paths are documentation/codegen mappings, not a WebSocket URI.

### Authentication

Auth is required by default: each device generates a token on first boot
(retrieve it with `bramble pair` over serial; see `docs/auth.md`). Clients
should send:

- `Authorization: Bearer <token>`

`?token=<token>` query auth is the supported mechanism for browser WebSocket clients, which cannot set request headers. Non-browser clients should use the `Authorization` header instead, because URLs can leak via logs/history.

Connections without credentials may call only `bramble.ping` and `bramble.getVersion`; every other method returns error `-1005` (`Unauthorized`). Wrong credentials close the WebSocket with code 1008.

Server-push notifications (`bramble.on*`) are delivered only to authenticated connections; an unauthenticated connection receives none. The single exception is a device whose owner has explicitly disabled auth, where every connection receives notifications (`rpc_auth_notify_filter` in `components/rpc/rpc_auth.c`). The same gating applies on BLE, where notifications are withheld until the token handshake succeeds.

Browser connections without a valid token are additionally subject to an `Origin` allowlist: same-origin always passes; other origins must be added via `bramble.setAllowedOrigins`. Presenting the valid token bypasses the Origin check.

## Location policy RPC contract (hybrid privacy-first)

### `bramble.setLocationConfig`

Sets persisted location policy and optional manual coordinates.

Accepted params (all optional for partial updates):

- `enabled` (bool)
- `default_tier` (string): `full | coarse | presence | off`
- `interval_s` (number)
- `source` (string): `gps | manual | hybrid`
- `lat` (number), `lon` (number): manual fallback coordinates
- `contact_rules` (array of objects):
  - `address` (8-char hex string)
  - `enabled` (bool, optional)
  - `tier` (string, optional)
  - `interval_s` (number, optional)
- `channel_targets` (array of objects):
  - `channel` (number)
  - `enabled` (bool, optional)
  - `tier` (string, optional)
  - `interval_s` (number, optional)

Response:

```json
{ "ok": true }
```

Compatibility notes:

- Existing `default_tier` and `interval_s` fields remain supported.
- Contact rules are stored and read only from canonical `lcr_XXXXXXXX` keys.
- Legacy `lc_XXXXXXXX` contact keys are no longer read or maintained.

### `bramble.getConfig` (location section)

`bramble.getConfig.result.location` now includes:

- `enabled` (bool)
- `tier` (string)
- `default_tier` (string)
- `interval_s` (number)
- `source` (string)
- `lat` / `lon` (number, when manual coordinates are present)
- `contact_rules` (array)
- `channel_targets` (array)

### `bramble.getPeerLocations`

`bramble.getPeerLocations.result` includes:

- `peerLocations` (array of canonical location peers)

Each peer object uses canonical fields:

- `addr` (8-char uppercase hex string)
- `name` (string)
- `tier` (`full | coarse | presence | off`)
- `position` (object or `null`)
- `online` (bool)
- `lastUpdatedMs` (number)

## `bramble.sendBroadcast` response fields

Current firmware returns snake_case fields:

- `broadcast_id` (string): broadcast correlation ID (8-char uppercase hex)
- `status` (`sent`)
- `broadcast` (boolean)
- `channel` (integer)
- `fragmented` (boolean)
- `fragments_total` (integer, only when fragmented)
- `max_bytes` (integer)
- `actual_bytes` (integer)

## Notification channel: `bramble.onBroadcastDelivery`

Current firmware emits `bramble.onBroadcastDelivery` notifications with:

- `recipient` (string): recipient node address (8-char uppercase hex)
- `broadcast_id` (string): broadcast correlation ID
- `status` (`delivered`)
- `rssi_at_dest` (integer, dBm)
- `relayPath` (optional array in path-sampled mode)

---

## RPC Method Reference (from `main/rpc_methods.c`)

Methods below are registered in firmware via `rpc_register(...)`. The
CI-enforced source of truth for the full method list is
[`api/openapi.yaml`](../../api/openapi.yaml) (checked against the firmware
registry by `scripts/check-rpc-contract.sh`); this page explains the
commonly used methods. Not yet documented here: `bramble.getPeerVerification`,
`bramble.setPeerVerified`, `bramble.otaStatus`, `bramble.injectInput`,
`bramble.screenshot`.

### Status / Info

#### `bramble.getStatus`

- Description: Returns high-level node runtime status.
- Params: none (`{}`).
- Response fields: `address` (string), `firmware_version` (string), `protocol_version` (string), `hardware` (string), `radio_ok` (bool), `peers` (number), `beacon_tx` (number), `beacon_rx` (number), `packets_tx` (number), `packets_rx` (number), `uptime_s` (number), `free_heap` (number), `battery_mv` (number), `battery_pct` (number), `charging` (string: `"unknown"`/`"no"`/`"yes"`), `present` (bool), `gps_available` (bool), `gps_enabled` (bool), `gps_state` (string), `gps_sats_in_view` (number), `gps_sats_tracked` (number), `gps_sats_used` (number), `gps_snr_max_dbhz` (number), `gps_fix_quality` (number), `supports_delivery_event_sync` (bool), `identity_pins` (number), `identity_conflicts` (number), `identity_sig_failures` (number), `identity_addr_mismatches` (number), `identity_unendorsed` (number), `identity_expired` (number).
- `present` reports whether the battery backend initialized, not whether this particular read succeeded, so treat a 0 `battery_mv` as no reading even when `present` is true.
- `gps_state` is one of `absent`, `no_signal`, `acquiring`, `fix`. The satellite counts and `gps_fix_quality` are always present and are 0 on a board without a receiver, so a client distinguishes "no receiver" from "zero satellites" on `gps_available`, never on a count. See [Diagnosing a node with no fix](#diagnosing-a-node-with-no-fix).
- Example:

```json
{"jsonrpc":"2.0","id":1,"method":"bramble.getStatus","params":{}}
```

#### `bramble.getDiagnostics`

- Description: Returns heap, task-stack and backpressure diagnostics, plus raw GPS feed counters on a board with a receiver.
- Params: optional `include_heap_dump` (bool); when true the node also writes a heap dump to its own console, which the response does not carry.
- Response fields: `uptime_s` (number), `free_heap` (number), `heap` (object: `internal_free`, `internal_largest_free_block`, `internal_min_ever_free`, `dma_free`, `dma_largest_free_block`, `psram_free`, `psram_min_ever_free`), `task_stack_hwm` (array of `{task, hwm_words, hwm_bytes}`), `backpressure` (object: `flood_relay_drops`, `probe_ingress`), and on a GPS-capable board `gps_rx_bytes`, `gps_rx_lines`, `gps_chip`, `gps_rx_overruns`, `gps_rx_errors`, `gps_rx_disabled`, `gps_rx_rearm_fail`.
- Example:

```json
{"jsonrpc":"2.0","id":2,"method":"bramble.getDiagnostics","params":{"include_heap_dump":true}}
```

#### `bramble.getWifiStatus`

- Description: Returns Wi-Fi mode/connectivity state.
- Params: none.
- Response fields: `mode` (string), `connected` (bool), `ssid` (string, optional), `ip` (string), `rssi` (number).
- Example:

```json
{"jsonrpc":"2.0","id":3,"method":"bramble.getWifiStatus","params":{}}
```

#### `bramble.getIdentity`

- Description: Returns device identity.
- Params: none.
- Response fields: `address` (string), `public_key_hash` (string), `name` (string).
- Example:

```json
{"jsonrpc":"2.0","id":4,"method":"bramble.getIdentity","params":{}}
```

#### `bramble.getVersion`

- Description: Returns firmware/protocol/build metadata.
- Params: none.
- Response fields: `firmware` (string), `protocol` (string), `hardware` (string), `supportsDeliveryEventSync` (bool, when enabled).
- Example:

```json
{"jsonrpc":"2.0","id":5,"method":"bramble.getVersion","params":{}}
```

#### `bramble.getBattery`

- Description: Returns battery telemetry.
- Params: none.
- Response fields: `percentage` (number), `voltage_mv` (number), `charging` (string: `"unknown"`/`"no"`/`"yes"`, optional), `present` (bool, optional; init status, not per-read: treat a 0 voltage as no reading even when true).
- Example:

```json
{"jsonrpc":"2.0","id":6,"method":"bramble.getBattery","params":{}}
```

#### `bramble.getStorageInfo`

- Description: Returns storage details (board-dependent).
- Params: none.
- Response fields: `sd_present` (bool), `sd_total_kb` (number), `sd_free_kb` (number).
- Example:

```json
{"jsonrpc":"2.0","id":7,"method":"bramble.getStorageInfo","params":{}}
```

#### `bramble.getGpsPosition`

- Description: Returns the latest GPS fix plus the satellite state behind it.
- Params: none.
- Response fields, on a fix: `valid` (bool), `lat` (number), `lon` (number), `alt` (number), `speed_kmh` (number), `heading_deg` (number), `accuracy_m` (number), `timestamp` (number).
- Response fields, on both branches: `state` (string), `sats_in_view` (number), `sats_tracked` (number), `sats_used` (number), `snr_max_dbhz` (number), `fix_quality` (number). A `valid: false` response therefore still says whether anything is reaching the receiver.
- A board without a GNSS receiver returns error `-1004` (method not supported), which a client reads as "no receiver" rather than as zero satellites.
- Example:

```json
{"jsonrpc":"2.0","id":8,"method":"bramble.getGpsPosition","params":{}}
```

#### Diagnosing a node with no fix

`state` and the counts split "no fix" into the three cases that need different
responses in the field:

- `no_signal` with `sats_in_view` 0: nothing is reaching the receiver. Suspect
  a disconnected or damaged antenna, an unpowered or miswired module, or
  jamming. Cross-check `gps_rx_lines` in `bramble.getDiagnostics`: zero lines
  means the module is not talking at all, nonzero means it is talking and
  hearing nothing.
- `no_signal` with `sats_in_view` above 0: the almanac predicts satellites and
  none are being heard. The receiver is alive and its sky view or antenna path
  is not. Moving to open sky is the first test.
- `acquiring` with a low `snr_max_dbhz` (below roughly 25): marginal signal.
  Obstruction, indoor use, or a poorly placed antenna.
- `acquiring` with a healthy `snr_max_dbhz` (roughly 35 and above): a cold
  start in progress. A receiver with no almanac takes minutes rather than
  seconds; leave it under open sky and watch `sats_used` climb.
- `absent`: the board has no receiver, or GPS is switched off by preference.
  Check `gps_available` and `gps_enabled` in `bramble.getStatus`.

Every value in this group describes what the receiver is reporting, not what it
reported at some earlier point. A receiver that says its fix is invalid leaves
`fix` on that sentence, and a receiver that stops sending NMEA at all drops to
`no_signal` with zero counts within 30 seconds. A node carried from a place it
fixed to a place where it hears nothing therefore reports the second place.

### Messaging

#### `bramble.sendMessage`

- Description: Sends a unicast message.
- Params: `to` (string 8-hex addr), `text` (string), optional `channel` (number), `wantAck` (bool).
- Response fields: `packetId` (string), `status` (string), `fragmented` (bool), `fragments_total` (number, optional), `max_bytes` (number), `actual_bytes` (number), `channel` (number).
- Example:

```json
{"jsonrpc":"2.0","id":10,"method":"bramble.sendMessage","params":{"to":"A1B2C3D4","text":"hello"}}
```

#### `bramble.sendBroadcast`

- Description: Sends a broadcast message.
- Params: `text` (string), optional `channel` (number, usually public).
- Response fields: `broadcast_id` (string), `status` ("sent"), `broadcast` (bool), `channel` (number), `fragmented` (bool), `fragments_total` (number, optional), `max_bytes` (number), `actual_bytes` (number).
- Example:

```json
{"jsonrpc":"2.0","id":11,"method":"bramble.sendBroadcast","params":{"text":"net check"}}
```

#### `bramble.getMessages`

- Description: Returns stored message history.
- Params: optional `limit` (number), `since_id` (number).
- Response fields: `messages` (array of message objects with `id`, `from`, `text`, `timestamp_ms`, `status`, `broadcast`, `channel`).
- Example:

```json
{"jsonrpc":"2.0","id":12,"method":"bramble.getMessages","params":{"limit":50}}
```

#### `bramble.getDeliveryEvents`

- Description: Returns persisted ACK/broadcast delivery replay events.
- Params: optional `sinceEventSeq|since_event_seq` (number), `limit` (number).
- Response fields: `supported` (bool), `next_event_seq` (number), `events` (array).
- Example:

```json
{"jsonrpc":"2.0","id":13,"method":"bramble.getDeliveryEvents","params":{"since_event_seq":100}}
```

### Network

#### `bramble.getNeighbors`

- Description: Returns current direct neighbors.
- Params: none.
- Response fields: `neighbors` (array; includes `address`, `rssi`, `snr`, `last_seen_ms`, `deliveryRate`, `airtimeRemaining`).
- Example:

```json
{"jsonrpc":"2.0","id":20,"method":"bramble.getNeighbors","params":{}}
```

#### `bramble.getRoutes`

- Description: Returns routing table entries.
- Params: none.
- Response fields: `routes` (array; `dest`, `next_hop`, `hops`, `metric`, `state`).
- Example:

```json
{"jsonrpc":"2.0","id":21,"method":"bramble.getRoutes","params":{}}
```

#### `bramble.getAirtime`

- Description: Returns airtime budget status.
- Params: none.
- Response fields: tier budget objects and totals (remaining/max/refill values).
- Example:

```json
{"jsonrpc":"2.0","id":22,"method":"bramble.getAirtime","params":{}}
```

#### `bramble.ping`

- Description: Lightweight liveness RPC.
- Params: optional `nonce` (string/number).
- Response fields: `ok` (bool), optional `nonce` echo.
- Example:

```json
{"jsonrpc":"2.0","id":23,"method":"bramble.ping","params":{"nonce":"abc123"}}
```

#### `bramble.sendProbe`

- Description: Starts active neighbor probe sweep.
- Params: optional `rounds` (number), `timeout_ms` (number).
- Response fields: `probe_id` (string), `status` (string).
- Example:

```json
{"jsonrpc":"2.0","id":24,"method":"bramble.sendProbe","params":{}}
```

### Configuration

#### `bramble.getConfig`

- Description: Returns full persisted runtime config.
- Params: none.
- Response fields: `node_name` (string), `radio` (object), `channels` (array), `default_channel` (number), `mailbox` (bool), `location` (object).
- Example:

```json
{"jsonrpc":"2.0","id":30,"method":"bramble.getConfig","params":{}}
```

#### `bramble.setRadio`

- Description: Updates radio config.
- Params: `frequency_mhz` (number), `sf` (number), `bw_hz` (number), `coding_rate` (number/string), `tx_power_dbm` (number).
- Response fields: `ok` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":31,"method":"bramble.setRadio","params":{"frequency_mhz":915.0,"sf":9,"bw_hz":125000,"tx_power_dbm":17}}
```

#### `bramble.setWifiConfig`

- Description: Provisions WiFi station credentials over any RPC transport
  (serial, WebSocket, BLE), so a first-boot device does not need its
  on-device UI or the AP-mode captive portal to join a network. The
  password is write-only: it is persisted to NVS but never echoed back by
  this or any other method. There is no live station reconfigure path
  today, so this always returns `applied: "reboot_required"`; call
  `bramble.reboot` afterward to apply the new credentials. Returns
  `not-supported` on hardware with no WiFi radio (the nRF52840 target and
  the Linux simulator).
- Params: `ssid` (string, 1-32 chars), optional `password` (string, 0-64
  chars; omit or send empty for an open network), optional `mode` (string,
  only `"sta"` is accepted; any other value is rejected).
- Response fields: `ok` (bool), `applied` (string: `"live"` or
  `"reboot_required"`; currently always `"reboot_required"`).
- Example:

```json
{"jsonrpc":"2.0","id":36,"method":"bramble.setWifiConfig","params":{"ssid":"my-network","password":"changeme123"}}
```

#### `bramble.setNodeName`

- Description: Sets local node display name.
- Params: `name` (string).
- Response fields: `ok` (bool), `name` (string).
- Example:

```json
{"jsonrpc":"2.0","id":32,"method":"bramble.setNodeName","params":{"name":"ridge-01"}}
```

#### `bramble.setBacklight`

- Description: Sets display backlight (board-dependent).
- Params: `level` (number, typically 0-255).
- Response fields: `ok` (bool), `level` (number).
- Example:

```json
{"jsonrpc":"2.0","id":33,"method":"bramble.setBacklight","params":{"level":128}}
```

#### `bramble.sleep`

- Description: Requests deep/light sleep.
- Params: optional `seconds` (number), `mode` (string), `wake_gpio` (number).
- Response fields: `ok` (bool), `sleeping` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":34,"method":"bramble.sleep","params":{"seconds":60}}
```

#### `bramble.reboot`

- Description: Reboots the node.
- Params: none.
- Response fields: `ok` (bool) before restart.
- Example:

```json
{"jsonrpc":"2.0","id":35,"method":"bramble.reboot","params":{}}
```

### Channels

#### `bramble.addChannel`

- Description: Adds a channel entry.
- Params: `name` (string), optional `psk` (string).
- Response fields: `ok` (bool), `index` (number).
- Example:

```json
{"jsonrpc":"2.0","id":40,"method":"bramble.addChannel","params":{"name":"team","psk":"secret"}}
```

#### `bramble.removeChannel`

- Description: Removes a channel by index.
- Params: `index` (number).
- Response fields: `ok` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":41,"method":"bramble.removeChannel","params":{"index":1}}
```

#### `bramble.setDefaultChannel`

- Description: Sets default TX channel index.
- Params: `index` (number).
- Response fields: `ok` (bool), `index` (number).
- Example:

```json
{"jsonrpc":"2.0","id":42,"method":"bramble.setDefaultChannel","params":{"index":0}}
```

### Auth

#### `bramble.setAuthToken`

- Description: Sets or clears the RPC auth bearer token (WS and BLE).
- Params: `token` (string; minimum 16 bytes; empty to disable auth as a persisted explicit opt-out).
- Response fields: `ok` (bool).
- Errors: `-32602` for non-empty tokens shorter than 16 bytes or 128 bytes and longer.
- Example:

```json
{"jsonrpc":"2.0","id":50,"method":"bramble.setAuthToken","params":{"token":"my-secret-token-16b"}}
```

#### `bramble.getAuthToken`

- Description: Returns currently configured auth token.
- Params: none.
- Response fields: `token` (string), `enabled` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":51,"method":"bramble.getAuthToken","params":{}}
```

#### `bramble.setAllowedOrigins`

- Description: Replaces the WebSocket `Origin` allowlist applied to tokenless connections (extra origins beyond same-origin, which always passes).
- Params: `origins` (array of full-origin strings, e.g. `"https://app.example.com"`; empty array clears the list; entries must contain `://` or be the literal `"null"`).
- Response fields: `ok` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":52,"method":"bramble.setAllowedOrigins","params":{"origins":["https://app.example.com"]}}
```

#### `bramble.getAllowedOrigins`

- Description: Returns the configured extra origins.
- Params: none.
- Response fields: `origins` (array of strings).
- Example:

```json
{"jsonrpc":"2.0","id":53,"method":"bramble.getAllowedOrigins","params":{}}
```

### Network key (control-plane MACs)

See `docs/network-key-provisioning.md` for the operator flow.

#### `bramble.generateNetworkKey`

- Description: Mints a fresh entropy-gated 32-byte network key on the device, provisions this node with it, and returns the raw key so the operator can copy it to the rest of the fleet. Re-keys an already-provisioned node. Fails closed (provisions nothing) on entropy failure. Authenticated callers only.
- Params: none.
- Response fields: `key` (string, 64 hex), `fingerprint` (string, 8 hex).
- Example:

```json
{"jsonrpc":"2.0","id":54,"method":"bramble.generateNetworkKey","params":{}}
```

#### `bramble.setNetworkKey`

- Description: Provisions the per-fleet network key behind the RREP/RERR/ACK/delivery-receipt/beacon control-plane MACs. Persisted to NVS, applied immediately (beacon subkey re-derived live). Write-only secret: never read back. Authenticated callers only.
- Params: `key` (string, raw 32-byte key as 64 hex characters).
- Response fields: `ok` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":55,"method":"bramble.setNetworkKey","params":{"key":"<64 hex chars>"}}
```

#### `bramble.getNetworkKeyStatus`

- Description: Reports whether a network key is provisioned plus a one-way fingerprint (`SHA256(key)[0:4]`, 8 lowercase hex). Unprovisioned nodes report the all-zero fingerprint `"00000000"` and are inert on the control plane (no public-PSK fallback).
- Params: none.
- Response fields: `provisioned` (bool), `fingerprint` (string).
- Example:

```json
{"jsonrpc":"2.0","id":56,"method":"bramble.getNetworkKeyStatus","params":{}}
```

### Trust anchor

See `docs/trust-anchor.md` for the enrollment ceremony.

#### `bramble.setAnchor`

- Description: Provisions the fleet trust anchor's Ed25519 public key. The device only ever holds the anchor public key, never the private key. Persisted to NVS. Authenticated callers only.
- Params: `anchor_pubkey` (string, 64 hex characters).
- Response fields: `ok` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":57,"method":"bramble.setAnchor","params":{"anchor_pubkey":"<64 hex chars>"}}
```

#### `bramble.setEndorsement`

- Description: Provisions this node's own anchor-signed endorsement certificate. Verified against this node's identity key and the provisioned anchor before persisting; triggers a fresh attestation on success. Authenticated callers only.
- Params: `not_after` (string), `endorsement_sig` (string).
- Response fields: `ok` (bool).
- Errors: `-32602` when no anchor is provisioned, `not_after` is 0, fields are malformed, or the signature does not verify.
- Example:

```json
{"jsonrpc":"2.0","id":58,"method":"bramble.setEndorsement","params":{"not_after":"18446744073709551615","endorsement_sig":"<128 hex chars>"}}
```

#### `bramble.getAnchorStatus`

- Description: Reports whether a trust anchor is provisioned, the anchor fingerprint (`SHA256(anchor_pub)[0:4]`, present only when anchored), and whether this node holds an endorsement cert.
- Params: none.
- Response fields: `anchored` (bool), `anchor_fingerprint` (string, optional), `endorsed` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":59,"method":"bramble.getAnchorStatus","params":{}}
```

### Location

#### `bramble.setLocationConfig`

- Description: Sets the node's location-sharing policy (enable, default tier, interval, source, manual coordinates). Full contract in the "Location policy RPC contract" section above.
- Params: see above.
- Response fields: `ok` (bool).

#### `bramble.getPeerLocations`

- Description: Returns known peer locations (and own position when available). Full response shape in the "Location policy RPC contract" section above.
- Params: none.
- Response fields: `peers` (array).

#### `bramble.setLocationContact`

- Description: Sets per-contact location sharing override.
- Params: `address` (string), optional `enabled` (bool), `tier` (string), `interval_s` (number).
- Response fields: `ok` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":60,"method":"bramble.setLocationContact","params":{"address":"A1B2C3D4","tier":"coarse","interval_s":300}}
```

#### `bramble.removeLocationContact`

- Description: Removes per-contact location rule.
- Params: `address` (string).
- Response fields: `ok` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":61,"method":"bramble.removeLocationContact","params":{"address":"A1B2C3D4"}}
```

#### `bramble.shareLocationOnce`

- Description: Sends a one-shot location update to a peer.
- Params: `address` (string), optional `tier` (string).
- Response fields: `ok` (bool), `queued` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":62,"method":"bramble.shareLocationOnce","params":{"address":"A1B2C3D4"}}
```

### Audio

#### `bramble.playTone`

- Description: Plays a local tone (board-dependent).
- Params: `freq_hz` (number), `duration_ms` (number), optional `volume` (number).
- Response fields: `ok` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":70,"method":"bramble.playTone","params":{"freq_hz":1000,"duration_ms":250}}
```

#### `bramble.setVolume`

- Description: Sets output volume (board-dependent).
- Params: `volume` (number).
- Response fields: `ok` (bool), `volume` (number).
- Example:

```json
{"jsonrpc":"2.0","id":71,"method":"bramble.setVolume","params":{"volume":8}}
```

#### `bramble.setMuted`

- Description: Toggles local audio mute.
- Params: `muted` (bool).
- Response fields: `ok` (bool), `muted` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":72,"method":"bramble.setMuted","params":{"muted":true}}
```

#### `bramble.getAudioStatus`

- Description: Returns audio state.
- Params: none.
- Response fields: `volume` (number), `muted` (bool), `available` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":73,"method":"bramble.getAudioStatus","params":{}}
```

### Advanced

#### `bramble.setMailbox`

- Description: Enables/disables mailbox mode.
- Params: `enabled` (bool).
- Response fields: `ok` (bool), `enabled` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":80,"method":"bramble.setMailbox","params":{"enabled":true}}
```

#### `bramble.setBroadcastTelemetryMode`

- Description: Configures broadcast delivery telemetry sampling.
- Params: `mode` (string, e.g. `off|sampled|all`), optional `sample_rate` (number).
- Response fields: `ok` (bool), `mode` (string).
- Example:

```json
{"jsonrpc":"2.0","id":81,"method":"bramble.setBroadcastTelemetryMode","params":{"mode":"sampled","sample_rate":25}}
```

#### `bramble.otaUpdate`

- Description: Starts OTA from an artifact path resolved against the device's allowlisted OTA origin. Raw URLs are rejected. The image must carry a valid signature trusted by the running firmware and pass the anti-rollback floor.
- Params: `path` (string, relative artifact path), optional `allow_downgrade` (bool).
- Response fields: `ok` (bool), `note` (string), `url` (string, resolved), `partition` (string), `error` (string when ok=false), `last_error` (string, failure reason from the previous attempt).
- Example:

```json
{"jsonrpc":"2.0","id":82,"method":"bramble.otaUpdate","params":{"path":"stable/v1.4.0/heltec-v3/bramble.bin"}}
```

#### `bramble.otaGetOrigin`

- Description: Reports the allowlisted OTA origin and anti-rollback state.
- Params: none.
- Response fields: `ok` (bool), `origin` (string), `default_origin` (string), `overridden` (bool), `version_floor` (string, when recorded), `running_version` (string).
- Example:

```json
{"jsonrpc":"2.0","id":83,"method":"bramble.otaGetOrigin","params":{}}
```

#### `bramble.otaSetOrigin`

- Description: Overrides or resets the allowlisted OTA origin (https base URL; http only on CONFIG_BRAMBLE_OTA_ALLOW_HTTP builds).
- Params: `origin` (string) or `reset` (bool).
- Response fields: `ok` (bool), `origin` (string), `overridden` (bool), `error` (string when ok=false).
- Example:

```json
{"jsonrpc":"2.0","id":84,"method":"bramble.otaSetOrigin","params":{"origin":"https://mirror.example/ota/"}}
```

#### `bramble.setTrafficDebug`

- Description: Configures traffic debug capture.
- Params: `enabled` (bool), optional `include_tx` (bool), `include_rx` (bool), `sample_rate` (0-100).
- Response fields: `ok` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":83,"method":"bramble.setTrafficDebug","params":{"enabled":true,"include_tx":true,"include_rx":true,"sample_rate":25}}
```

#### `bramble.getTrafficDebug`

- Description: Returns traffic debug config and buffer stats.
- Params: none.
- Response fields: `enabled` (bool), `include_tx` (bool), `include_rx` (bool), `sample_rate` (number), `capacity` (number), `count` (number), `dropped` (number).
- Example:

```json
{"jsonrpc":"2.0","id":84,"method":"bramble.getTrafficDebug","params":{}}
```

#### `bramble.getTrafficEvents`

- Description: Returns replay window from traffic debug ring buffer.
- Params: optional `since_seq` (number), `limit` (number).
- Response fields: `next_seq` (number), `events` (array).
- Example:

```json
{"jsonrpc":"2.0","id":85,"method":"bramble.getTrafficEvents","params":{"since_seq":0,"limit":100}}
```

#### `bramble.setBeaconPolicy`

- Description: Updates periodic beaconing policy.
- Params: optional `enabled` (bool), `interval_ms` (number), `jitter_pct` (number), `min_neighbors` (number).
- Response fields: `ok` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":86,"method":"bramble.setBeaconPolicy","params":{"enabled":true,"interval_ms":30000}}
```

#### `bramble.getBeaconPolicy`

- Description: Returns effective beacon policy.
- Params: none.
- Response fields: `enabled` (bool), `interval_ms` (number), `jitter_pct` (number), `min_neighbors` (number).
- Example:

```json
{"jsonrpc":"2.0","id":87,"method":"bramble.getBeaconPolicy","params":{}}
```

#### `bramble.setFloodTransport`

- Description: Toggles the unicast flood transport: when on, unicast DATA not addressed to this node relays through the multi-hop flood engine instead of the reactive route-lookup forward. Default off. NVS-persisted.
- Params: `enabled` (bool).
- Response fields: `ok` (bool), `enabled` (bool).
- Example:

```json
{"jsonrpc":"2.0","id":88,"method":"bramble.setFloodTransport","params":{"enabled":true}}
```

#### `bramble.setFloodHopLimit`

- Description: Sets the hop limit stamped on freshly-originated flood DATA (and its flooded ACK). Clamped to 1..32; the effective value is echoed back and persisted. Default 8. Separate from the reactive routing hop budget.
- Params: `hops` (number).
- Response fields: `ok` (bool), `hops` (number).
- Example:

```json
{"jsonrpc":"2.0","id":89,"method":"bramble.setFloodHopLimit","params":{"hops":12}}
```
