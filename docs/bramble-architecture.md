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
| `crypto` | `components/crypto/` | AES-256-GCM, X25519, HKDF |
| `packet` | `components/packet/` | Packet framing, serialization, type definitions |
| `routing` | `components/routing/` | AODV route discovery, forwarding, beacon, route maintenance, route metrics |
| `channel` | `components/channel/` | Named group channels (key derivation, message encrypt/decrypt, public channel) |
| `security` | `components/security/` | Session management, key exchange, dummy traffic |
| `reliability` | `components/reliability/` | ACKs, retransmission, delivery receipts, flow control |
| `fragment` | `components/fragment/` | Message fragmentation and reassembly |
| `airtime` | `components/airtime/` | Duty cycle tracking and TX priority queue |
| `dedup` | `components/dedup/` | Duplicate packet detection (sliding-window bloom filter) |
| `identity` | `components/identity/` | Node identity, X25519 keypair generation and storage |
| `timesync` | `components/timesync/` | Stratum-based mesh time synchronization |
| `radio` | `components/radio/` | SX1262 driver, airtime math, and the budget-gated TX gate (single transmit path) |
| `mailbox` | `components/mailbox/` | Store-and-forward buffer for offline destinations |
| `location` | `components/location/` | Private location sharing with tiered privacy |
| `group` | `components/group/` | Group DM management and key derivation |
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
| `0x0A` | `PKT_TYPE_DATA` | Encrypted data payload | variable |
| `0x0B` | `PKT_TYPE_STORE_REQUEST` | Request a mailbox node to store a message | variable |
| `0x0C` | `PKT_TYPE_STORE_ACK` | Acknowledgement of successful mailbox storage | fixed |
| `0x0D` | `PKT_TYPE_MAILBOX_DELIVERY` | Delivery of stored message to now-online destination | variable |
| `0x0E` | `PKT_TYPE_MAILBOX_QUERY` | Query a mailbox node for messages | fixed |
| `0x12` | `PKT_TYPE_PROBE` | Network reachability probe | variable |
| `0x13` | `PKT_TYPE_PROBE_ACK` | Probe acknowledgement | variable |
| `0x14` | `PKT_TYPE_LOCATION` | Location sharing packet | variable |

Implementation status: the mesh RX dispatcher (`mesh_process_rx_packet` in `main/mesh_task.c`) currently handles `ACK`, `RREQ`, `RREP`, `RERR`, `BEACON`, `DELIVERY_RECEIPT`, `DATA`, `LOCATION`, `PROBE`, and `PROBE_ACK`. The remaining defined types (`KEY_EXCHANGE` and the four mailbox types) are not sent or handled by the firmware today. Type codes `0x08`, `0x09`, `0x0F`, `0x10`, and `0x11` (formerly `CONGESTION`, `TIME_SYNC`, `EMERGENCY`, `EMERGENCY_CANCEL`, and `CODED`) are retired: that machinery was deleted unshipped. Mailbox store-and-forward works without its dedicated packet types: relays store undeliverable `DATA` packets and flush them when the destination's beacon is heard.

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

Key sizes: `BRAMBLE_KEY_SIZE` = 32 bytes, `BRAMBLE_NONCE_SIZE` = 12 bytes, `BRAMBLE_TAG_SIZE` = 16 bytes.

---

### `packet`

**Files:** `packet.c`

Defines all packet structs and provides serialize/deserialize functions for each packet type. Serialization is always to/from raw byte arrays (no protobuf, no dynamic allocation). All functions return `ESP_OK` on success or `ESP_ERR_INVALID_SIZE` if the buffer is too small.

The `bramble_header_t` struct maps directly onto the 12-byte on-wire format. Higher-level structs (e.g., `bramble_beacon_t`, `bramble_rreq_t`) embed the header as their first field.

---

### `routing`

**Files:** `beacon.c`, `discovery.c`, `forwarding.c`, `routing.c`

Implements AODV-inspired reactive unicast routing:

1. **Route discovery** (`discovery.c`): Broadcasts RREQ with encrypted source address; destination unicasts RREP along reverse path. Expanding-ring search: hop limit 4 on the first attempt, 8 on the retries at +5 s and +15 s, each retry under a fresh query_id so dedup on nodes that heard an earlier attempt cannot swallow it. Relays delay RREQ rebroadcasts by a random 50-300 ms so same-hop relays do not collide with each other. Cached routes have soft (30s inactivity) and hard (300s) timeouts.
2. **Forwarding** (`forwarding.c`): Looks up next-hop from route table; decrements `hop_limit`; emits RERR on unknown destination.
3. **Beacons** (`beacon.c`): Periodic 60s neighbor discovery beacons carrying node status, public key hash, HMAC'd with pairwise session key for known peers.
4. **Route metric** (`routing.c`): penalty-accumulating link metric (RSSI/SNR), first-arrival selection within a flood, better-metric arbitration between discovery attempts at `route_install`.

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

A per-destination sliding window (max 4 unacknowledged packets) with AIMD adjustment is implemented and tested in `reliability.c`, but is not yet wired into the mesh task; the CONGESTION packet type was removed unshipped. The retry/ACK machinery above is live.

---

### `fragment`

**Files:** `fragment.c`

Splits messages larger than the LoRa MTU (~240 bytes usable after header and crypto overhead) into multiple fragments. Fragments share a `packet_id` and are tagged with `FLAG_FRAG` bits (first/middle/last). Reassembly times out after 30s if not all fragments arrive.

---

### `airtime`

**Files:** `airtime_budget.c`

Per-tier token-bucket airtime budget with continuous (proportional) refill, consumed by the radio TX gate:

- **Budget** (`airtime_budget.c`): Four lanes with hourly base budgets of 36 s critical, 18 s normal, 18 s broadcast, and 12 s receipt (`AIRTIME_BUDGET_*_MS`). An adaptive profile scales the maxima by mesh size (`airtime_budget_set_mesh_size`), CRITICAL may borrow a capped share of NORMAL (`AIRTIME_BORROW_CAP_PCT`, 25%), and a regulatory duty-cycle cap from the frequency plan scales everything down when the region enforces one (`airtime_budget_set_duty_cap`; EU868 = 1%). See [airtime-budget-v2.md](airtime-budget-v2.md).

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

**Files:** `timesync.c`

Stratum-based mesh time synchronization inspired by NTP:
- Sync rides the beacon: each beacon carries `network_time` and a stratum/confidence field, consumed by `timesync_handle_sync` on beacon receipt (`main/mesh_task.c`). The dedicated TIME_SYNC packet type was removed unshipped; the beacon is the only sync transport.
- GPS-equipped nodes are stratum 0; other nodes adopt the best (lowest stratum) time source they hear and become stratum+1.
- Convergence to ±1–2s across the mesh.

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
