# Bramble Architecture

A technical reference for the Bramble LoRa mesh networking protocol stack.

---

## Overview

Bramble is a privacy-first LoRa mesh networking protocol targeting ESP32-S3 + SX1262 hardware. It provides encrypted, multi-hop communication without central infrastructure. Message payloads on configured channels are encrypted with AES-256-GCM, route-request sources are pseudonymized, and the channel a message belongs to is hidden inside the ciphertext. There are no pairwise end-to-end keys today, and several control-plane packets are unauthenticated; [SECURITY-MODEL.md](SECURITY-MODEL.md) is the authoritative statement of what is and is not protected.

The codebase is organized as ESP-IDF components. Each component is self-contained with a clean public API exposed through its `include/` header. Components depend on each other only through these interfaces: there are no circular dependencies.

See also:
- [`docs/COMPARISON.md`](COMPARISON.md): Comparison with Meshtastic and MeshCore
- [`docs/bramble-anomaly-detection.md`](bramble-anomaly-detection.md): Anomaly detection subsystem
- [`simulator/README.md`](../simulator/README.md): Network simulator

---

## Design Goals

1. **Privacy by design**: Source addresses, channel IDs, and routing metadata are hidden from relay nodes and passive observers wherever possible.
2. **Scalability**: Reactive (on-demand) routing for unicast DMs; controlled flood routing for channels. O(path_length) transmissions per DM after route discovery, not O(N).
3. **Airtime discipline**: Token-bucket duty cycle enforcement prevents channel monopolization and ensures regulatory compliance.
4. **Tiered reliability**: Applications choose between fire-and-forget, acknowledged, and critical delivery; each tier has appropriate retry and flow-control behavior.
5. **Embedded constraints**: Integer-only arithmetic, static allocation, no heap after init, minimal RAM footprint (~127 KB total).

---

## Component Map

| Component | Path | Purpose |
|-----------|------|---------|
| `crypto` | `components/crypto/` | AES-256-GCM, X25519, HKDF, anti-replay window |
| `packet` | `components/packet/` | Packet framing, serialization, type definitions |
| `routing` | `components/routing/` | AODV route discovery, forwarding, beacon, route maintenance, route metrics |
| `channel` | `components/channel/` | Named group channels (key derivation, message encrypt/decrypt, public channel) |
| `security` | `components/security/` | Session management, key exchange, dummy traffic |
| `reliability` | `components/reliability/` | ACKs, retransmission, delivery receipts, flow control |
| `fragment` | `components/fragment/` | Message fragmentation and reassembly |
| `airtime` | `components/airtime/` | Duty cycle tracking and TX priority queue |
| `dedup` | `components/dedup/` | Duplicate packet detection (sliding-window bloom filter) |
| `identity` | `components/identity/` | Node identity, X25519 keypair generation and storage |
| `timesync` | `components/timesync/` | Stratum-based mesh time synchronization, anti-replay |
| `radio` | `components/radio/` | SX1262 driver, airtime math, and the budget-gated TX gate (single transmit path) |
| `mailbox` | `components/mailbox/` | Store-and-forward buffer for offline destinations |
| `emergency` | `components/emergency/` | Emergency beacon state machine and tracking |
| `location` | `components/location/` | Private location sharing with tiered privacy |
| `group` | `components/group/` | Group DM management and key derivation |
| `coding` | `components/coding/` | XOR network coding for bidirectional relay |
| `msg_store` | `components/msg_store/` | Message persistence (SPIFFS-backed message history) |
| `audio` | `components/audio/` | Audio playback (tones, alerts) |
| `battery` | `components/battery/` | Battery voltage monitoring and reporting |
| `gps` | `components/gps/` | GPS/GNSS driver and NMEA parser |
| `wifi` | `components/wifi/` | Wi-Fi station/AP mode management |
| `sdcard` | `components/sdcard/` | SD card storage driver |
| `bramble_probe` | `components/bramble_probe/` | Network reachability probe sweep |
| `freq_plan` | `components/freq_plan/` | Regional frequency plan definitions |
| `nvs_keys` | `components/nvs_keys/` | Central NVS namespace/key registry |
| `rpc` | `components/rpc/` | JSON-RPC dispatcher, method registration, and auth policy (default-on token, unauthenticated allowlist, notification gating) |
| `traffic_debug` | `components/traffic_debug/` | Runtime TX/RX traffic capture and airtime attribution telemetry |
| `board_config` | `components/board_config/` | Per-board hardware capability definitions |
| `display` | `components/display/` | OLED status display (hardware-dependent) |
| `ble` | `components/ble/` | BLE interface + key backup (hardware-dependent) |
| `ota` | `components/ota/` | Signed OTA updates: image signature verification, origin allowlist, soft anti-rollback (hardware-dependent) |
| `ui` | `components/ui/` | JSON-RPC control interface |
| `ui_graphics` | `components/ui_graphics/` | LVGL-based GUI (T-Deck Plus: chat, nodes, settings screens) |
| `keyboard` | `components/keyboard/` | T-Deck Plus I2C keyboard driver (ESP32-C3 sub-MCU) |
| `trackball` | `components/trackball/` | T-Deck Plus hall-effect trackball driver |
| `bramble_touch` | `components/bramble_touch/` | T-Deck Plus touchscreen driver |
| `button` | `components/button/` | Physical button input handling |

---

## JSON-RPC API Notes (Current Firmware)

The runtime source of truth for available RPC methods is `main/rpc_methods.c`.
The OpenAPI document (`api/openapi.yaml`) is kept in sync with that registry,
enforced in CI by `scripts/check-rpc-contract.sh`.

Access control (see [auth.md](auth.md) and [SECURITY-MODEL.md](SECURITY-MODEL.md)):
- Auth is required by default on WebSocket and BLE; serial is the unauthenticated pairing bootstrap.
- Unauthenticated connections may call only `bramble.ping` and `bramble.getVersion` (`components/rpc/rpc_auth.c`) and receive no server-push notifications.
- Tokenless browser connections are subject to a WebSocket `Origin` allowlist (`main/ws_origin.c`, enforced in `main/ws_server.c`).

Notable wire-format details clients must honor:
- `bramble.getAirtime` returns flat fields (`critical_remaining_ms`, etc.)
- `bramble.sendMessage` returns `packetId` as a hex string
- `bramble.setRadio` expects snake_case keys (`frequency_mhz`, `bw_hz`, `tx_power_dbm`, `coding_rate`)
- location contact methods use `address` as 8-char hex string

---

## Packet Format

### Common Header (12 bytes)

All Bramble packets begin with a 12-byte header:

```
Offset  Size  Field
0       1     version   (always 0x01)
1       1     type      (PKT_TYPE_*)
2       1     flags     (see Flag Bits below)
3       1     hop_limit (decremented at each relay; drop at 0)
4       4     dest_addr (destination node address; 0xFFFFFFFF = broadcast)
8       4     packet_id (random ID for dedup and ACK matching)
```

All multi-byte fields are **big-endian** (network byte order; `put_be32`/`get_be32` in `components/packet/packet.c`).

### Flag Bits (header byte 2)

| Bits | Mask | Name | Meaning |
|------|------|------|---------|
| 7–6 | `0xC0` | `FLAG_TIER` | Reliability tier: 00=broadcast, 01=normal, 10=critical |
| 5 | `0x20` | `FLAG_ACK_REQ` | Sender requests an end-to-end ACK |
| 4 | `0x10` | `FLAG_RECEIPT` | Sender requests a delivery receipt with relay path |
| 3 | `0x08` | `FLAG_CHANNEL` | Payload is a channel message (flood routing) |
| 2 | `0x04` | `FLAG_ENCRYPT` | Payload is encrypted |
| 1–0 | `0x03` | `FLAG_FRAG` | Fragment indicator (00=none, 01=first, 10=middle, 11=last) |

`HEADER_FLAG_EMERGENCY` (`0x04` in the flags byte) is defined to signal emergency relay priority, but nothing in the firmware sets or checks it today; every transmission, emergency-flagged or not, goes through the budget-gated TX path.

### Packet Types

| Value | Name | Description | Size |
|-------|------|-------------|------|
| `0x01` | `PKT_TYPE_ACK` | End-to-end acknowledgement | 23–55 bytes |
| `0x02` | `PKT_TYPE_RREQ` | Route Request (AODV route discovery) | 30 bytes |
| `0x03` | `PKT_TYPE_RREP` | Route Reply | 34 bytes |
| `0x04` | `PKT_TYPE_RERR` | Route Error (broken link notification) | 24 bytes |
| `0x05` | `PKT_TYPE_BEACON` | Node status beacon | 44+ bytes (name optional) |
| `0x06` | `PKT_TYPE_KEY_EXCHANGE` | X25519 key exchange (3-step) | 101 bytes |
| `0x07` | `PKT_TYPE_DELIVERY_RECEIPT` | Path-tracing delivery receipt | 22–54 bytes |
| `0x08` | `PKT_TYPE_CONGESTION` | Congestion notification | 20 bytes |
| `0x09` | `PKT_TYPE_TIME_SYNC` | Mesh time synchronization | 24 bytes |
| `0x0A` | `PKT_TYPE_DATA` | Encrypted data payload | variable |
| `0x0B` | `PKT_TYPE_STORE_REQUEST` | Request a mailbox node to store a message | variable |
| `0x0C` | `PKT_TYPE_STORE_ACK` | Acknowledgement of successful mailbox storage | fixed |
| `0x0D` | `PKT_TYPE_MAILBOX_DELIVERY` | Delivery of stored message to now-online destination | variable |
| `0x0E` | `PKT_TYPE_MAILBOX_QUERY` | Query a mailbox node for messages | fixed |
| `0x0F` | `PKT_TYPE_EMERGENCY` | Emergency beacon (plaintext, broadcast) | 17–49 bytes |
| `0x10` | `PKT_TYPE_EMERGENCY_CANCEL` | Cancel an active emergency (authenticated) | 12 bytes |
| `0x11` | `PKT_TYPE_CODED` | XOR network-coded packet (two components) | variable |
| `0x12` | `PKT_TYPE_PROBE` | Network reachability probe | variable |
| `0x13` | `PKT_TYPE_PROBE_ACK` | Probe acknowledgement | variable |
| `0x14` | `PKT_TYPE_LOCATION` | Location sharing packet | variable |

Implementation status: the mesh RX dispatcher (`mesh_process_rx_packet` in `main/mesh_task.c`) currently handles `ACK`, `RREQ`, `RREP`, `RERR`, `BEACON`, `DELIVERY_RECEIPT`, `DATA`, `LOCATION`, `PROBE`, and `PROBE_ACK`. The remaining defined types (`KEY_EXCHANGE`, `CONGESTION`, `TIME_SYNC`, the four mailbox types, the two emergency types, and `CODED`) are not sent or handled by the firmware today; their components exist and are unit-tested, but they are not wired into the mesh task. Mailbox store-and-forward works without its dedicated packet types: relays store undeliverable `DATA` packets and flush them when the destination's beacon is heard.

### Beacon Flags

The `bramble_beacon_t.flags` field carries per-feature capability bits:

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | `BEACON_FLAG_MAILBOX` | Node is willing to store messages for offline peers |

---

## Component Reference

### `crypto`

**Files:** `crypto_esp.c` (hardware AES on ESP32-S3), `crypto_host.c` (mbedTLS for host tests)

Provides the cryptographic primitives used throughout the stack:

- **AES-256-GCM**: `crypto_aes256gcm_encrypt` / `_decrypt`: AEAD with 12-byte nonce, 16-byte auth tag
- **X25519**: `crypto_x25519`: Diffie-Hellman key exchange
- **HKDF-SHA256**: `crypto_hkdf`: Key derivation from shared secrets and context strings
- **Random**: `crypto_random`: Cryptographically secure random bytes
- **Anti-replay**: 64-bit sliding window in `timesync/anti_replay.c` (implemented and tested, but currently has no callers in the firmware; see SECURITY-MODEL.md known gaps)

Key sizes: `BRAMBLE_KEY_SIZE` = 32 bytes, `BRAMBLE_NONCE_SIZE` = 12 bytes, `BRAMBLE_TAG_SIZE` = 16 bytes.

---

### `packet`

**Files:** `packet.c`

Defines all packet structs and provides serialize/deserialize functions for each packet type. Serialization is always to/from raw byte arrays (no protobuf, no dynamic allocation). All functions return `ESP_OK` on success or `ESP_ERR_INVALID_SIZE` if the buffer is too small.

The `bramble_header_t` struct maps directly onto the 12-byte on-wire format. Higher-level structs (e.g., `bramble_beacon_t`, `bramble_rreq_t`) embed the header as their first field.

---

### `routing`

**Files:** `beacon.c`, `beacon_routes.c`, `channel_flood.c`, `discovery.c`, `forwarding.c`, `routing.c`, `route_metric.c`

Implements AODV-inspired reactive unicast routing:

1. **Route discovery** (`discovery.c`): Broadcasts RREQ with encrypted source address; destination unicasts RREP along reverse path. Cached routes have soft (30s inactivity) and hard (300s) timeouts.
2. **Forwarding** (`forwarding.c`): Looks up next-hop from route table; decrements `hop_limit`; emits RERR on unknown destination.
3. **Beacons** (`beacon.c`): Periodic 60s neighbor discovery beacons carrying node status, public key hash, HMAC'd with pairwise session key for known peers.
4. **Route maintenance** (`beacon_routes.c`): Processes RERR packets, invalidates stale routes, triggers re-discovery.
5. **Channel flooding** (`channel_flood.c`): Hop-limited (default 3) re-broadcast for channel messages. Dedup prevents loops.
6. **Adaptive routing metrics** (`route_metric.c`): See dedicated section below.

Privacy note: RREQ packets encrypt the source address: intermediate relay nodes cannot determine who initiated a route discovery.

---

### `channel`

**Files:** `channel_key.c`, `channel_msg.c`, `public_channel.c`

Provides named encrypted group channels:

- **Key derivation** (`channel_key.c`): Derives a 32-byte AES key from a PSK string using HKDF. Epoch-based key ratchet: `channel_advance_epoch()` advances the key to provide backward secrecy (old keys are deleted after ratchet).
- **Message encrypt/decrypt** (`channel_msg.c`): Inner plaintext structure includes `channel_id(1) + epoch(2) + app_type(1) + src_addr(4)` prepended to user data before AES-256-GCM encryption. The channel ID and source address are therefore hidden from non-members. Trial decryption tries all known channels in constant time to prevent timing side-channels; epoch catch-up tries up to 256 epoch advances per channel.
- **Public channel** (`public_channel.c`): See dedicated section below.

**Bug fix (2026-02-17):** `channel_msg_decrypt` had a pointer bug where `found_info.data` was set to `ciphertext + CHANNEL_MSG_OVERHEAD` instead of `pt + CHANNEL_MSG_OVERHEAD`. This caused the caller to receive a pointer into the ciphertext buffer rather than the decrypted plaintext. Fixed by re-decrypting the winning channel after the constant-time loop and copying the plaintext data portion back into the ciphertext buffer (which the caller owns), then setting `found_info.data` to `NULL` to indicate the catchup path, and re-decrypting once more in the normal path.

---

### `security`

**Files:** `security.c`, `dummy_traffic.c`

Manages session keys and the 3-step key exchange:

1. **Initiate**: Alice sends `PKT_TYPE_KEY_EXCHANGE` with ephemeral X25519 pubkey
2. **Respond**: Bob sends his ephemeral + long-term pubkeys
3. **Confirm**: Alice sends final auth tag proving she holds the derived session key

Session key = HKDF(ephemeral-DH ‖ static-DH, "bramble-session"). Keys rotate every 24h or 65,536 messages.

Implementation status: the session/key-exchange machinery is component-level only. `PKT_TYPE_KEY_EXCHANGE` is never sent and never handled on the wire, and direct messages are encrypted with the shared channel key, not pairwise session keys (see SECURITY-MODEL.md).

**Dummy traffic** (`dummy_traffic.c`): Optionally emits random-length encrypted packets to defend against traffic analysis (volume and timing correlation).

---

### `reliability`

**Files:** `reliability.c`

Three delivery tiers:

| Tier | Retries | Timeout | Notes |
|------|---------|---------|-------|
| Broadcast | 0 | none | Fire-and-forget |
| Normal | 3 | 2s base, exponential backoff with ±25% jitter | End-to-end ACK required |
| Critical | 8 | 3s base, exponential backoff with ±25% jitter | Delivery receipt + full relay path |

(Constants: `tier_max_retries` / `tier_base_delay_ms` in `components/reliability/reliability.c`; jitter applied in `main/mesh_task.c`.)

A per-destination sliding window (max 4 unacknowledged packets) with AIMD adjustment is implemented and tested in `reliability.c`, but is not yet wired into the mesh task; `PKT_TYPE_CONGESTION` is never sent or received (see the packet-type implementation status note). The retry/ACK machinery above is live.

---

### `fragment`

**Files:** `fragment.c`

Splits messages larger than the LoRa MTU (~240 bytes usable after header and crypto overhead) into multiple fragments. Fragments share a `packet_id` and are tagged with `FLAG_FRAG` bits (first/middle/last). Reassembly times out after 30s if not all fragments arrive.

---

### `airtime`

**Files:** `airtime_budget.c`, `tx_queue.c`

Per-tier token-bucket airtime budget with continuous (proportional) refill, consumed by the radio TX gate:

- **Budget** (`airtime_budget.c`): Four lanes with hourly base budgets of 36 s critical, 18 s normal, 18 s broadcast, and 12 s receipt (`AIRTIME_BUDGET_*_MS`). An adaptive profile scales the maxima by mesh size (`airtime_budget_set_mesh_size`), CRITICAL may borrow a capped share of NORMAL (`AIRTIME_BORROW_CAP_PCT`, 25%), and a regulatory duty-cycle cap from the frequency plan scales everything down when the region enforces one (`airtime_budget_set_duty_cap`; EU868 = 1%). See [airtime-budget-v2.md](airtime-budget-v2.md).
- **TX queue** (`tx_queue.c`): Priority queue ordered by tier. Currently exercised only by host tests; the live transmit path is the radio TX gate below.

Every transmission is admitted and debited by the TX gate in `components/radio/tx_gate.c` (see the `radio` section); there is no budget-exempt transmit path.

---

### `dedup`

**Files:** `dedup.c`

Sliding window of the last 64 `packet_id` values seen. Incoming packets with a duplicate ID are silently dropped before any processing. The window is per-node-address to handle ID collisions between different senders.

---

### `identity`

**Files:** `identity.c`

Generates and persists a node's X25519 keypair on first boot (stored in NVS). The 4-byte node address is derived as `SHA-256(long_term_pubkey)[0:4]`. Provides `identity_get_addr()` and `identity_sign()` for use by other components.

---

### `timesync`

**Files:** `timesync.c`, `anti_replay.c`

Stratum-based mesh time synchronization inspired by NTP:
- Sync rides the beacon: each beacon carries `network_time` and a stratum/confidence field, consumed by `timesync_handle_sync` on beacon receipt (`main/mesh_task.c`). The dedicated `PKT_TYPE_TIME_SYNC` packet is defined but never sent or handled.
- GPS-equipped nodes are stratum 0; other nodes adopt the best (lowest stratum) time source they hear and become stratum+1.
- Convergence to ±1–2s across the mesh.

**Anti-replay** (`anti_replay.c`): 64-bit sliding window keyed on `(src_addr, packet_id)` to reject replayed packets.

---

### `radio`

**Files:** `radio_esp.c`, `sx1262.c`, `radio_airtime.c`, `tx_gate.c`, `tx_gate_esp.c`, `radio_mock.c`

`radio_esp.c` / `sx1262.c`: SX1262 driver (SPI, IRQ handling, CAD). The raw transmit primitive `radio_transmit_raw` is declared only in `radio_internal.h`; nothing outside the component can transmit without going through the TX gate.

`tx_gate.c` / `tx_gate_esp.c`: The single budget-gated transmit path. Every packet (data, retries, forwards, beacons, routing control, ACKs, receipts, probes) is classified into a `tx_kind_t`, mapped to a budget tier, costed with real time-on-air math, checked against the airtime budget, passed through listen-before-talk (up to 3 CAD attempts with randomized exponential backoff), transmitted, and debited. A beacon-sized reserve in the broadcast lane guarantees the next beacon's tokens cannot be drained by broadcast data.

`radio_airtime.c`: Calculates on-air time for a packet given spreading factor, bandwidth, and coding rate (Semtech AN1200.13). Used by the TX gate to cost every transmission and by the simulator's radio model.

`radio_mock.c`: Software radio stub for host-side unit tests and the network simulator. Accepts/delivers packets via callbacks rather than hardware SPI.

---

### `traffic_debug` (v0.3: 2026-02-22)

**Files:** `components/traffic_debug/traffic_debug.{h,c}`

Runtime traffic observability and airtime analysis telemetry:

- **Event capture:** Records TX/RX packet metadata (packet type, length, RSSI, airtime tier, timestamp) into a 512-event ring buffer.
- **Classification:** Categorizes packets into `beacon`, `timesync`, `routing`, `ack`, `chat`, `maintenance`, or `other` based on packet type.
- **Airtime attribution:** Maps each packet to its airtime bucket (`broadcast`, `normal`, `critical`) for efficiency analysis.
- **Serial JSONL sink:** Emits one JSON line per event to UART for offline capture and analysis.
- **Runtime config:** Enable/disable via RPC (`bramble.setTrafficDebug`), with configurable TX/RX inclusion and sampling rate (0-100%).
- **RPC/WebSocket API:**
  - `bramble.setTrafficDebug`: configure debug mode (persisted to NVS)
  - `bramble.getTrafficDebug`: query config + buffer state (capacity, count, dropped)
  - `bramble.getTrafficEvents`: pull events from ring buffer (incremental via `since_seq`)
  - `bramble.onTrafficEvent`: real-time WebSocket notifications (live stream)

**Use case:** Measure airtime usage by category (e.g., "beacons consume 40% of broadcast budget"), identify retry storms, tune beacon intervals, and explain broadcast-budget drain in field deployments.

**Design notes:**
- Event emission never blocks the radio critical path (drop events before blocking).
- Default mode: disabled (no overhead when not in use).
- Sampling: configurable 0-100% sampling rate for high-traffic environments.
- Privacy: captures metadata only (no payloads, addresses, or message content in v1).

---

## New Components (v0.2: 2026-02-17)

The following seven components were added as part of the simulator component integration milestone. All are implemented, unit-tested, and included in the test suite. Not all are wired into the live mesh path: see the implementation-status note under Packet Types for which packet types the firmware actually sends and handles today.

---

### Public Channel (`components/channel/public_channel.{h,c}`)

The **Bramble Common** public channel is channel index 0, always available on every node without out-of-band key distribution.

**Key material:** Derived from SHA-256(`"bramble-default"`) via the standard `channel_derive_key()` path. The well-known PSK means any node can participate without configuration.

**TX rate limiting (token bucket):**
- Burst capacity: 3 messages
- Refill rate: 1 token per 30 seconds
- `public_channel_can_send(now_ms)` returns `false` when the bucket is empty

**RX rate limiting:** Per-source sliding-window rate limiter (`public_channel_rx_check(src_addr, now_ms)`) guards against a single noisy sender flooding the channel.

**Default hop limit:** 3 hops (short-range community broadcast).

**Initialization:** `public_channel_init(channels, num_channels)` is called once at boot. It sets `channels[0]` with the well-known key and sets `num_channels` to at least 1.

---

### Mailbox / Store-and-Forward (`components/mailbox/`)

Allows nodes to store messages destined for currently-offline peers and deliver them when the destination comes online.

**Buffer:** 32 entries, each up to 200-byte payload.

**Admission control:**
- Per-destination cap: 8 entries
- Per-source cap: 8 entries
- FIFO eviction when buffer full (oldest entry displaced)
- TTL: 24 hours (`MAILBOX_TTL_MS`)

**Protocol integration:**
- `BEACON_FLAG_MAILBOX` (`0x01`) in the beacon flags field advertises willingness to store
- `PKT_TYPE_STORE_REQUEST` (`0x0B`): sender asks a mailbox node to store a message
- `PKT_TYPE_STORE_ACK` (`0x0C`): mailbox confirms storage
- `PKT_TYPE_MAILBOX_DELIVERY` (`0x0D`): mailbox delivers stored message when destination returns
- `PKT_TYPE_MAILBOX_QUERY` (`0x0E`): destination queries a mailbox node for pending messages

**API:**
```c
void mailbox_init(mailbox_t *mb);
int  mailbox_store(mailbox_t *mb, src, dest, payload, len, packet_id, now_ms);
int  mailbox_retrieve(mailbox_t *mb, dest_addr, out[], max_out);
void mailbox_purge_expired(mailbox_t *mb, now_ms);
```

---

### Emergency Beacon (`components/emergency/`)

A three-state machine for distress signaling:

```
INACTIVE ──activate──► ACTIVE ──cancel──► COOLDOWN ──timeout──► INACTIVE
                           │                                         ▲
                           └──────────── 24h auto-timeout ──────────┘
```

**States:**
- `EMERGENCY_STATE_INACTIVE` (0): normal operation
- `EMERGENCY_STATE_ACTIVE` (1): transmitting emergency beacons every 30s
- `EMERGENCY_STATE_COOLDOWN` (2): 15-minute cool-down after cancel before re-activation

**Timing constants:**
- Auto-timeout: 24 hours (`EMERGENCY_AUTO_TIMEOUT_MS`)
- Beacon interval: 30 seconds (`EMERGENCY_BEACON_INTERVAL_MS`)
- Cooldown: 15 minutes (`EMERGENCY_COOLDOWN_MS`)
- Minimum interval between activations: 1 hour (`EMERGENCY_MIN_ACTIVATION_MS`)

**Packet format:**
- `PKT_TYPE_EMERGENCY` (`0x0F`): Plaintext beacon carrying `src_addr`, `latitude_e7`, `longitude_e7`, `altitude_m`, `battery_pct`, `timestamp`, and an optional 32-byte short message. Minimum serialized size: 17 bytes.
- `PKT_TYPE_EMERGENCY_CANCEL` (`0x10`): Authenticated cancel (4-byte truncated HMAC-SHA256). Size: 12 bytes.

**Header flag:** `HEADER_FLAG_EMERGENCY` (`0x04`) in the packet header flags byte. All relay nodes forward emergency packets unconditionally, bypassing airtime budget checks.

**Multi-node tracking:** Each node maintains a table of up to 8 active emergencies received from other nodes (`emergency_record_received`, `emergency_record_cancel`).

**API:**
```c
void emergency_init(emergency_manager_t *mgr);
int  emergency_activate(mgr, lat_e7, lon_e7, alt_m, battery, msg, now_ms);
int  emergency_cancel(mgr, now_ms);
bool emergency_is_active(mgr);
void emergency_tick(mgr, now_ms);        /* call periodically */
bool emergency_should_beacon(mgr, now_ms);
```

---

### Private Location Sharing (`components/location/`)

Encrypted per-contact location sharing with three privacy tiers. Location updates are sent as encrypted `PKT_TYPE_DATA` payloads over established pairwise sessions.

**Privacy tiers:**

| Tier | Constant | Payload | Size |
|------|----------|---------|------|
| Full | `LOCATION_TIER_FULL` (0) | lat, lon, alt, accuracy, speed, heading, timestamp | 17 bytes |
| Coarse | `LOCATION_TIER_COARSE` (1) | ~1 km grid square (lat/lon quantized to 0.01°) + low-res timestamp | 5 bytes |
| Presence | `LOCATION_TIER_PRESENCE` (2) | Online/offline status byte only | 1 byte |

**Position format (full, 17 bytes):**
```
lat_e7(4) + lon_e7(4) + alt_m(2) + accuracy_m(1) + speed_kmh(1) + heading_deg2(1) + timestamp(4)
```

**Sharing rules:**
- Per-contact sharing config: each contact has an assigned tier and an `auto_approve_requests` flag.
- Update triggers: time-based (default 5-minute interval) or distance-based (default 100 m threshold).
- `location_should_send(mgr, peer_addr, now_ms)` combines both triggers.

**Position cache:**
- Up to 16 cached peer positions (`LOCATION_MAX_CONTACTS`)
- TTL: 1 hour (`LOCATION_CACHE_TTL_MS`)
- `location_cache_purge(mgr, now_ms)` evicts stale entries

**API:**
```c
void location_init(location_manager_t *mgr);
int  location_add_contact(mgr, peer_addr, tier);
void location_set_position(mgr, pos);
int  location_serialize_full(pos, buf, buf_len);    /* → 17 bytes */
int  location_serialize_coarse(pos, buf, buf_len);  /* → 5 bytes */
```

---

### Group DMs (`components/group/`)

Encrypted group messaging for up to 8 members per group, with up to 8 simultaneous groups per node.

**Key derivation:** FNV-1a over the sorted member address list concatenated with the group name. Produces an 8-byte group ID and a 32-byte group key.
```
group_id || group_key = FNV-1a(sort(member_addrs) || name)
```

**Epoch-based key rotation:** After every 256 messages (`GROUP_EPOCH_ADVANCE_THRESHOLD`), `group_advance_epoch()` is called. The new key is derived from the old key + new epoch number, ensuring backward secrecy for group messages.

**Invite serialization:** Group invites are serialized as fixed-size packets (`GROUP_INVITE_SIZE` = `GROUP_ID_SIZE + GROUP_KEY_SIZE + GROUP_NAME_MAX + 2`) containing the group ID, current key, name, and epoch. Used for onboarding new members.

**Group lifecycle:**
```c
void group_init(group_manager_t *mgr);
int  group_create(mgr, name, creator_addr, member_addrs[], num_members, now_ms);
int  group_delete(mgr, group_id);
int  group_add_member(group, addr);
int  group_remove_member(group, addr);
bool group_is_member(group, addr);
void group_record_message(group);   /* advances epoch after threshold */
```

**Limits:**
- `GROUP_MAX_MEMBERS` = 8 members per group
- `GROUP_MAX_GROUPS` = 8 groups per node
- Group name max: 32 characters
- `GROUP_EPOCH_ADVANCE_THRESHOLD` = 256 messages

---

### Network Coding (`components/coding/`)

XOR-based network coding for bidirectional relay scenarios. When a relay node has two packets that need to travel in opposite directions to two neighbors who each already have one of the packets, it can XOR them together and send a single coded packet: both neighbors decode the packet they need using the one they already have.

**Encoding:** `coding_encode(pkt_a, len_a, id_a, pkt_b, len_b, id_b, coded_out, coded_len_out)` pads the shorter packet to `max(len_a, len_b)` and XOR-combines them. The coded header is prepended.

**Decoding:** `coding_decode(coded_data, coded_len, header, known_component, known_len, known_id, decoded_out, decoded_len_out)` XORs the coded payload against the known component to recover the other.

**Coded packet header** (`CODED_HEADER_MAX_SIZE` = 13 bytes):
```
num_components(1) + [packet_id(4) + orig_len(2)] × num_components
```

**Packet type:** `PKT_TYPE_CODED` (`0x11`)

**Reception cache:**
- Each node maintains a circular buffer of the last 32 packet IDs it has seen (`CODING_RECEPTION_CACHE`).
- `coding_record_packet(engine, packet_id)` adds to the local cache.
- `coding_record_neighbor_reception(engine, neighbor_addr, ids[], count)` updates per-neighbor knowledge from piggybacked reception reports.
- `coding_neighbor_has_packet(engine, neighbor_addr, packet_id)` is used to determine whether a coding opportunity exists.

**Opportunity detection:** `coding_find_opportunity(engine, &idx_a, &idx_b)` scans the queue for two packets where neighbor_a has packet_b and neighbor_b has packet_a. Returns 0 on success.

**Queue:** Up to 8 packets (`CODING_QUEUE_SIZE`) can wait up to 500ms (`CODING_OPPORTUNITY_WINDOW_MS`) for a coding partner before being flushed as uncoded.

**API:**
```c
void coding_init(coding_engine_t *engine);
void coding_record_packet(engine, packet_id);
int  coding_queue_packet(engine, data, len, packet_id, dest_addr, now_ms);
int  coding_find_opportunity(engine, &idx_a, &idx_b);
int  coding_encode(pkt_a, len_a, id_a, pkt_b, len_b, id_b, coded_out, coded_len_out);
int  coding_decode(coded_data, coded_len, header, known, known_len, known_id, out, out_len);
bool coding_can_decode(engine, header, known_id_out);
void coding_flush_expired(engine, now_ms);
```

---

### Adaptive Routing Metrics (`components/routing/route_metric.{h,c}`)

Composite route quality metric replacing the simple hop-count metric. Used by the AODV routing layer to select and maintain the best path to each destination.

**Composite metric (0–255, higher = better):**
```
metric = (link_quality  × 102 +
          delivery_rate ×  77 +
          airtime_score ×  51 +
          latency_score ×  26) / 256
```

Weights (must sum to 256 for integer-only arithmetic):
- Link quality (RSSI/SNR derived): **40%** (weight 102)
- Delivery rate (ACK success ratio, EMA-filtered): **30%** (weight 77)
- Airtime remaining (normalized to 0–255): **20%** (weight 51)
- Latency (RTT EMA, inverted): **10%** (weight 26)

**EMA filters** (exponential moving average, α = 1/8):
- `route_metric_update_delivery(current_rate, success)`: updates delivery rate on each ACK received or timeout
- `route_metric_update_latency(current_avg, new_sample)`: updates latency EMA

**Hysteresis:** Routes are only switched when the new metric exceeds the current by `METRIC_HYSTERESIS_THRESHOLD` (15 units) AND at least `METRIC_SWITCH_COOLDOWN_MS` (10 seconds) have elapsed since the last switch. This prevents oscillation on links of similar quality.

`route_metric_should_switch(current_metric, new_metric, last_switch_ms, now_ms)` encapsulates both checks.

**Airtime score:** `route_metric_airtime_score(remaining_ms, max_ms)` linearly maps remaining airtime budget to a 0–255 scale.

All operations use integer-only arithmetic (no floating point), suitable for ESP32-S3 without FPU dependency.

---

## Bug Fixes

### `channel_msg_decrypt` data pointer (2026-02-17)

**Symptom:** After successful channel message decryption, `channel_msg_info_t.data` pointed into the ciphertext buffer (at `ciphertext + CHANNEL_MSG_OVERHEAD`) instead of the decrypted plaintext buffer. Callers reading application data from the channel message would receive ciphertext bytes.

**Root cause:** The constant-time trial decryption loop stores decrypted plaintext in a local `pt[]` buffer on the stack. The result struct's `data` pointer was initialized to `ciphertext + CHANNEL_MSG_OVERHEAD` but was never updated to point to `pt + CHANNEL_MSG_OVERHEAD` after decryption succeeded.

**Fix:** After the constant-time loop identifies the winning channel, a final re-decryption copies `pt[CHANNEL_MSG_OVERHEAD:]` into `ciphertext[CHANNEL_MSG_OVERHEAD:]` (the caller-owned buffer), making the plaintext available at the expected address. For the epoch catch-up path (where `data = NULL`), the re-decryption also runs and sets the data pointer correctly.

**Location:** `components/channel/channel_msg.c`, function `channel_msg_decrypt()`.

---

## Changelog

### v0.2: 2026-02-17 (`feature/sim-component-integration`)

- **feat:** Added Public Channel (Bramble Common): channel 0 with well-known PSK, TX/RX rate limiters
- **feat:** Added Mailbox / Store-and-Forward: 32-entry buffer, TTL-based expiry, new packet types 0x0B–0x0E
- **feat:** Added Emergency Beacon: 3-state FSM, 24h auto-timeout, `HEADER_FLAG_EMERGENCY` bypass, packet types 0x0F–0x10
- **feat:** Added Private Location Sharing: 3 privacy tiers, 17-byte full / 5-byte coarse serialization, position cache
- **feat:** Added Group DMs: FNV-1a key derivation, epoch rotation every 256 messages, max 8 members/8 groups
- **feat:** Added Network Coding: XOR encode/decode, reception cache, coding opportunity detection, `PKT_TYPE_CODED` 0x11
- **feat:** Added Adaptive Routing Metrics: composite metric with EMA filters, hysteresis, integer-only arithmetic
- **fix:** `channel_msg_decrypt` data pointer pointed to ciphertext instead of decrypted plaintext
- **feat:** Added 7 new CMake test targets (test\_public\_channel, test\_mailbox, test\_emergency, test\_location, test\_group, test\_coding, test\_route\_metric)

### v0.1: 2026-02-16 (`master` baseline)

- Initial protocol stack: crypto, packet, routing, channel, security, reliability, fragment, airtime, dedup, identity, timesync, radio
- Network simulator with web UI, anomaly detection, and scenario runner
- 29 Unity test suites (host-side, no hardware required)
