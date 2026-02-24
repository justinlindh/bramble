# Bramble RPC API Notes

## WebSocket RPC endpoint

Bramble JSON-RPC over WebSocket uses the `/ws` endpoint.

- Example: `ws://192.168.4.1/ws`
- OpenAPI `/rpc/...` paths are documentation/codegen mappings, not a WebSocket URI.

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
