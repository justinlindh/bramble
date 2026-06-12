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

Legacy `?token=<token>` query auth is still accepted for compatibility (and is the only option for browser WebSocket clients), but is deprecated for everything else because URLs can leak via logs/history.

Connections without credentials may call only `bramble.ping` and `bramble.getVersion`; every other method returns error `-1005` (`Unauthorized`). Wrong credentials close the WebSocket with code 1008.

Browser connections are additionally subject to an `Origin` allowlist: same-origin always passes; other origins must be added via `bramble.setAllowedOrigins`.

## Location policy RPC contract (hybrid privacy-first)

### `bramble.setLocationConfig`

Sets persisted location policy and optional manual coordinates.

Accepted params (all optional for partial updates):

- `enabled` (bool)
- `default_tier` (string) — `full | coarse | presence | off`
- `interval_s` (number)
- `source` (string) — `gps | manual | hybrid`
- `lat` (number), `lon` (number) — manual fallback coordinates
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

## Complete RPC Method Reference (from `main/rpc_methods.c`)

All methods below are registered in firmware via `rpc_register(...)`.

### Status / Info

#### `bramble.getStatus`
- Description: Returns high-level node runtime status.
- Params: none (`{}`).
- Response fields: `uptime_s` (number), `neighbors` (number), `routes` (number), `radio_ok` (bool), `battery_pct` (number, when available), `gps` (object, when available).
- Example:
```json
{"jsonrpc":"2.0","id":1,"method":"bramble.getStatus","params":{}}
```

#### `bramble.getDiagnostics`
- Description: Returns heap/task diagnostics.
- Params: optional `include_tasks` (bool).
- Response fields: `heap_free` (number), `heap_min` (number), `tasks` (array, optional).
- Example:
```json
{"jsonrpc":"2.0","id":2,"method":"bramble.getDiagnostics","params":{"include_tasks":true}}
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
- Response fields: `pct` (number), `mv` (number, optional), `charging` (bool, optional).
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
- Description: Returns latest GPS fix.
- Params: none.
- Response fields: `valid` (bool), `lat` (number), `lon` (number), `alt_m` (number), `accuracy_m` (number).
- Example:
```json
{"jsonrpc":"2.0","id":8,"method":"bramble.getGpsPosition","params":{}}
```

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
- Description: Replaces the WebSocket `Origin` allowlist (extra origins beyond same-origin, which always passes).
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

### Location

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
- Description: Starts OTA from URL.
- Params: `url` (string), optional `sha256` (string).
- Response fields: `ok` (bool), `status` (string).
- Example:
```json
{"jsonrpc":"2.0","id":82,"method":"bramble.otaUpdate","params":{"url":"https://example.com/bramble.bin"}}
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
