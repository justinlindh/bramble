# Firmware Config Implementation Plan
*Created: 2026-02-18*

## Overview
The Bramble webapp has full UI for all config features, but 11 of 12 firmware RPC action methods are stubbed. This plan implements them in priority order, with NVS persistence where appropriate.

## Priority Order
1. **Radio Settings** (setRadio) — most impactful, enables tuning for range/speed
2. **Channel Management** (addChannel, removeChannel, setDefaultChannel) — needed for private groups
3. **Network Probe** (sendProbe) — differentiator, broadcast delivery tracking
4. **Mailbox** (setMailbox) — store-and-forward for offline nodes
5. **Location** (setLocationConfig, setLocationContact, removeLocationContact, shareLocationOnce, getPeerLocations) — GPS features

## Dependencies
- Radio: `components/radio/sx1262.c`, `components/freq_plan/`
- Channels: `components/channel_key/`, `components/channel_msg/`, `components/public_channel/`
- Probe: `components/probe/` (exists in simulator, needs firmware wiring)
- Mailbox: `components/mailbox/` (exists in simulator)
- Location: `components/location/` (exists in simulator)

---

## Phase 1: Radio Settings (setRadio)
*Estimate: 1 hour*

### Task 1.1: Parse and validate radio params
- File: `main/rpc_methods.c` → `handle_set_radio()`
- Parse JSON params: `frequency_mhz`, `sf`, `bw_hz`, `tx_power_dbm`
- Validate against freq_plan limits (frequency bounds, max TX power, valid SF/BW combos)
- Return detailed error on invalid params

### Task 1.2: Apply radio config at runtime
- File: `main/mesh_task.c` — add `mesh_reconfigure_radio()` function
- Stop RX, reconfigure SX1262 (freq, SF, BW, TX power, coding rate), restart RX
- Use existing `sx1262_set_frequency()`, `sx1262_set_modulation_params()`, `sx1262_set_tx_params()`
- Must hold mutex during reconfiguration to prevent TX during change
- **Critical: ALL nodes in mesh must use same radio config or they can't communicate**

### Task 1.3: Persist radio config to NVS
- NVS namespace `"bramble_radio"`, keys: `"freq_mhz"`, `"sf"`, `"bw_khz"`, `"tx_power"`, `"cr"`
- On boot: check NVS first, fall back to freq_plan defaults
- Wire into `mesh_task_start()` radio init path

### Task 1.4: Add radio profile presets
- Predefined profiles in `radio_esp.c`: "long_range" (SF12/BW125), "balanced" (SF9/BW125), "fast" (SF7/BW250)
- Profile name stored in NVS, overridable by individual params
- Expose in getConfig response as `radio.profile`

---

## Phase 2: Channel Management
*Estimate: 1.5 hours*

### Task 2.1: Implement addChannel
- File: `main/rpc_methods.c` → `handle_add_channel()`
- Parse params: `name` (string, max 16 chars), `psk` (optional hex string or passphrase)
- If PSK provided: derive channel key via `channel_key_derive()` from existing component
- If no PSK: generate random key via `crypto_random()`
- Add to `s_channels[]` array in mesh_task (need accessor function)
- Persist to NVS: `"bramble_ch"` namespace, `"ch_count"`, `"ch_0_name"`, `"ch_0_key"`, etc.
- Return channel index on success

### Task 2.2: Implement removeChannel
- File: `main/rpc_methods.c` → `handle_remove_channel()`
- Parse params: `index` (number) or `name` (string)
- Cannot remove default channel (index 0 = public channel)
- Compact array, update NVS
- Return ok + updated channel count

### Task 2.3: Implement setDefaultChannel
- File: `main/rpc_methods.c` → `handle_set_default_channel()`
- Parse params: `index` (number)
- Swap channel to index 0 position (send/receive defaults to index 0)
- Update NVS ordering

### Task 2.4: Load channels from NVS on boot
- In `mesh_task_start()`: after public channel init, load additional channels from NVS
- Merge: public channel always at index 0, NVS channels appended
- Cap at MAX_CHANNELS (currently defined in channel types)

---

## Phase 3: Network Probe (sendProbe)
*Estimate: 45 minutes*

### Task 3.1: Implement sendProbe
- File: `main/rpc_methods.c` → `handle_send_probe()`
- Build and transmit a PROBE packet (type 0x12) via radio
- Probe = broadcast with TTL, nodes that receive it send PROBE_ACK (type 0x13)
- Use existing `bramble_header_t` with `PKT_TYPE_PROBE`
- Return probe_id for tracking

### Task 3.2: Handle probe responses
- File: `main/mesh_task.c` → add `case PKT_TYPE_PROBE:` and `case PKT_TYPE_PROBE_ACK:`
- On PROBE rx: if hop_limit > 0, forward + send ACK back
- On PROBE_ACK rx: record in a probe results table (address, hops, RSSI, latency)
- Emit `bramble.onProbeResult` notification via RPC

### Task 3.3: Wire probe results to getStatus or new RPC
- Add `bramble.getProbeResults` RPC method
- Return array of `{ address, hops, rssi, latency_ms, timestamp }`
- Clear results after 60s or on new probe

---

## Phase 4: Mailbox (setMailbox)
*Estimate: 30 minutes*

### Task 4.1: Implement setMailbox
- File: `main/rpc_methods.c` → `handle_set_mailbox()`
- Parse params: `enabled` (bool), optionally `max_stored` (number, default 20)
- Set flag in mesh_task state, persist to NVS
- When enabled: beacon includes BEACON_FLAG_MAILBOX (0x01)
- When receiving data for a destination that's a known neighbor but currently offline, store in message store for later delivery

### Task 4.2: Store-and-forward delivery
- In `handle_rx_packet()` for DATA packets: if dest is a known neighbor but not recently heard, queue for later
- When neighbor comes back online (beacon rx), flush stored messages
- Add `bramble.getMailbox` RPC for viewing queued messages

---

## Phase 5: Location Features
*Estimate: 1.5 hours*

### Task 5.1: Implement setLocationConfig
- File: `main/rpc_methods.c` → `handle_set_location_config()`
- Parse params: `enabled` (bool), `interval_s` (beacon interval), `default_tier` ("off"|"presence"|"zone"|"exact")
- Persist to NVS
- **No real GPS hardware yet** — this sets the config for when GPS is available
- For testing: accept manual coordinates via `setLocationConfig({ lat, lon })`

### Task 5.2: Implement location contacts
- `handle_set_location_contact()`: add peer address + tier to whitelist, persist to NVS
- `handle_remove_location_contact()`: remove from whitelist
- `handle_share_location_once()`: send a one-time location packet to specified peer
- Max 16 location contacts

### Task 5.3: Implement getPeerLocations
- Return known peer locations from received location beacons
- Store in a static array (peer_addr, lat, lon, tier, timestamp)
- Max 32 peer locations, expire after 1 hour

### Task 5.4: Location packet TX/RX
- On location beacon timer: build location packet with coordinates + tier
- Filter recipients based on contact whitelist
- On RX: parse location data, store in peer locations table
- Emit `bramble.onPeerLocation` notification

---

## Phase 6: Webapp Normalization Updates
*Estimate: 30 minutes*

### Task 6.1: Update normalizeConfig for new fields
- Radio profile in config response
- Channel count, hasPsk detection
- Location config fields

### Task 6.2: Handle new notifications
- `bramble.onProbeResult` → update store probe results
- `bramble.onPeerLocation` → update map markers

### Task 6.3: Error feedback in UI
- Currently stubbed RPCs return `{ok: false, note: "..."}` silently
- Webapp should show the error/note to the user
- Check if actions.ts handles non-exception error responses

---

## Validation
After each phase:
1. Build on GPU box (`scripts/flash.sh`)
2. Flash both boards
3. Verify via CLI (`bramble-cli` or WebSocket)
4. Test in webapp, screenshot and send to Justin
5. Commit + push to Gitea

## Notes
- All NVS writes should use a single namespace per feature area
- Radio changes affect the entire mesh — webapp should warn before applying
- Channel PSK should never be sent back in getConfig (security)
- Location is privacy-sensitive — OFF by default, explicit opt-in per peer
