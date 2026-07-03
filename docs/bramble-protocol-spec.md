# Bramble: A LoRa Mesh Networking Protocol for ESP32

**Version:** 0.1-draft  
**Date:** 2026-02-15  
**Status:** Design Document  
**Target Hardware:** ESP32-S3 + Semtech SX1262. Running targets today are Heltec WiFi LoRa 32 V3/V4 and LilyGo T-Deck Plus; the T-Beam listed in the original draft was never brought up.

> **Implementation status.** This is the founding design document. The firmware on `main` implements a subset of it and diverges in specifics; sections below carry short "Firmware reality" notes where the shipped code contradicts the design. For the current state, [bramble-architecture.md](bramble-architecture.md) describes the components as built, [SECURITY-MODEL.md](SECURITY-MODEL.md) is authoritative for the security posture, and `api/openapi.yaml` is the CI-enforced RPC contract.

---

## Table of Contents

1. [Overview & Motivation](#1-overview--motivation)
2. [Design Principles](#2-design-principles)
3. [Hardware Target & Physical Layer](#3-hardware-target--physical-layer)
4. [Packet Format](#4-packet-format)
5. [Node Identity & Key Management](#5-node-identity--key-management)
6. [Routing Protocol](#6-routing-protocol)
7. [Reliability Layer](#7-reliability-layer)
8. [Airtime Budget System](#8-airtime-budget-system)
9. [Time Synchronization](#9-time-synchronization)
10. [Security Analysis](#10-security-analysis)
11. [Resource Budget](#11-resource-budget)
12. [Implementation Roadmap](#12-implementation-roadmap)
13. [Future Enhancements](#13-future-enhancements)

---

## 1. Overview & Motivation

### 1.1 What Bramble Is

Bramble is a ground-up LoRa mesh networking protocol designed for reliable, private communication across large mesh networks (50–200+ nodes, up to 8 hops). It targets ESP32-based LoRa hardware and prioritizes privacy, intelligent routing, and tiered reliability over simplicity.

### 1.2 Why Not Meshtastic?

Meshtastic pioneered consumer LoRa mesh networking, but its protocol has fundamental architectural limitations that cannot be fixed incrementally:

| Problem | Meshtastic | Bramble |
|---|---|---|
| **Routing** | Naive flooding — every node rebroadcasts every packet. O(N) airtime per message. Network saturates at ~30 nodes. | Reactive AODV-style routing with cached routes. O(path_length) airtime per message after route discovery. |
| **Reliability** | "Best effort" with optional ACKs but no retry intelligence, no flow control, no congestion awareness. | Three-tier reliability model with exponential backoff, sliding window flow control, and congestion-aware scheduling. |
| **Privacy** | All nodes in a channel share one PSK. Any channel member can impersonate any other. Relay nodes see all content. | Per-node X25519 key exchange for DMs. Relay nodes route ciphertext they cannot decrypt. Privacy-preserving route discovery. |
| **Scalability** | Flooding creates O(N²) total transmissions per message. Beacon storms. Channel congestion above ~30 nodes. | Route-based forwarding scales to 200+ nodes. Adaptive airtime budgets prevent congestion. |
| **Airtime management** | None. Nodes transmit whenever they want. One chatty node can monopolize the channel. | Token-bucket airtime budget per node with priority queuing and congestion backoff. |

### 1.3 Design Goals

1. **50+ node meshes** with 6–8 hop paths, sustained operation
2. **Privacy by default** — DM content unreadable by relay nodes, metadata-minimized route discovery
3. **Tiered reliability** — guaranteed delivery for critical messages, fire-and-forget for telemetry
4. **ESP32 feasibility** — everything fits in ~320KB RAM, ~4MB flash
5. **No infrastructure required** — no GPS, no internet, no servers. Fully decentralized.

---

## 2. Design Principles

### 2.1 Privacy First

Every design decision passes through a privacy filter:
- **DM content** is end-to-end encrypted with per-pair X25519-derived keys. Relay nodes see only opaque ciphertext.
- **Route discovery** uses ephemeral query IDs. Intermediate nodes cannot correlate a route request to a specific source node. Only the destination can decrypt the source identity.
- **No global topology map.** Nodes know their neighbors and cached routes — never the full network graph.
- **Beacons expose only a node's public key hash**, not a stable human-readable identity.

### 2.2 Reliability Through Intelligence

Rather than "blast and pray" flooding, Bramble builds routing knowledge passively and uses it actively:
- **Delivery receipts include relay paths**, so senders passively learn network topology.
- **Neighbor quality metrics** (RSSI, SNR, success rate) inform route selection.
- **Congestion signals propagate** so senders back off before the network saturates.

### 2.3 ESP32-Constrained Design

Every data structure has a hard size cap. Every algorithm has bounded memory and CPU:
- Routing table: max 64 entries × 24 bytes = 1,536 bytes
- Neighbor table: max 32 entries × 20 bytes = 640 bytes
- TX queue: max 16 entries × 256 bytes = 4,096 bytes
- RX dedup buffer: max 256 entries × 8 bytes = 2,048 bytes
- Crypto key cache: max 32 entries × 64 bytes = 2,048 bytes
- Total protocol RAM overhead target: **< 25KB** (excluding application buffers)

### 2.4 Conserve Airtime

LoRa is slow. At SF10/125kHz, a 200-byte packet takes ~700ms to transmit. Every byte and every transmission must be justified:
- Route-based forwarding: only nodes on the path transmit (not the entire mesh).
- Compact binary packet format — no JSON, no protobuf, no padding.
- Piggybacked ACKs where possible to reduce standalone ACK packets.
- Adaptive duty cycle management to avoid regulatory and congestion issues.

---

## 3. Hardware Target & Physical Layer

### 3.1 Target Hardware

**Primary:** Heltec WiFi LoRa 32 V3
- MCU: ESP32-S3, 240 MHz dual-core, 512KB SRAM (~320KB usable), 4–8MB flash
- Radio: Semtech SX1262 (150 MHz – 960 MHz)
- Display: 0.96" OLED (SSD1306, 128×64)
- Battery: LiPo via onboard charger, deep sleep ~10µA

**Secondary:** LILYGO T-Beam S3 Supreme
- Same ESP32-S3 + SX1262
- GPS module (optional, not required by protocol)
- 18650 battery holder

**Radio Interface:** SPI to SX1262/SX1276. Bramble uses the radio in explicit header mode with CRC enabled.

### 3.2 LoRa Physical Layer Parameters

Bramble defines two radio profiles. All nodes in a mesh must use the same profile.

#### Profile: LongRange (default)

| Parameter | Value | Rationale |
|---|---|---|
| Frequency | 906.875 MHz | US ISM band (902–928 MHz), center of a common sub-band |
| Spreading Factor | SF10 | Good range (~10km LOS) with acceptable data rate. SF12 is too slow for mesh. |
| Bandwidth | 125 kHz | Standard, best sensitivity (-137 dBm at SF10) |
| Coding Rate | 4/6 (CR2) | Moderate FEC. 4/5 saves airtime but fails in noisy environments. 4/8 is too expensive. |
| TX Power | +22 dBm (SX1262 max) | Maximum legal US power for frequency hopping systems. Configurable down. |
| Preamble | 12 symbols | Reliable detection. Default 8 misses weak signals. |
| Sync Word | 0x1424 | Private network sync word (not LoRaWAN 0x3444) |
| CRC | Enabled | Hardware CRC-16 on all packets |
| Explicit Header | Enabled | Required for variable-length packets |
| Max Payload | 222 bytes | SX1262 limit with explicit header |

**Resulting characteristics (SF10, 125kHz, CR 4/6):**
- Bit rate: 3,125 bps (raw), ~2,083 bps (effective with CR)
- 100-byte payload: ~480ms airtime
- 200-byte payload: ~850ms airtime
- Link budget: ~154 dB (sensitivity -132 dBm + 22 dBm TX)
- Practical range: 3–8 km urban, 10–20 km rural/LOS

#### Profile: MediumRange

| Parameter | Value | Rationale |
|---|---|---|
| Frequency | 906.875 MHz | Same band |
| Spreading Factor | SF8 | Shorter range (~5km LOS), 4× faster throughput |
| Bandwidth | 250 kHz | Wider BW for speed |
| Coding Rate | 4/5 (CR1) | Minimum FEC, maximize throughput |
| TX Power | +22 dBm | Configurable |
| Preamble | 8 symbols | Adequate for shorter-range, higher-SNR links |

**Resulting characteristics (SF8, 250kHz, CR 4/5):**
- Bit rate: 12,500 bps (raw), ~10,000 bps (effective)
- 100-byte payload: ~120ms airtime
- 200-byte payload: ~210ms airtime
- Practical range: 1–3 km urban, 5–10 km rural/LOS

### 3.3 Channel Activity Detection (Listen-Before-Talk)

Before transmitting, nodes perform Listen-Before-Talk (LBT) using the SX1262's hardware Channel Activity Detection (CAD):

1. Configure CAD parameters: 2-symbol detection, peak sensitivity 22, minimum sensitivity 10, CAD-only exit mode.
2. Run CAD check (~5–8ms at SF9/BW125). If channel is clear, transmit immediately.
3. If channel busy, back off with randomized exponential delay: `base_ms × 2^attempt + random(0, base_ms × 2^attempt)`, capped at 300ms per attempt.
4. Retry CAD up to 3 attempts. If still busy after all attempts, transmit anyway to avoid starvation.

```
LBT_MAX_ATTEMPTS    = 3
LBT_BACKOFF_BASE_MS = 50
LBT_BACKOFF_MAX_MS  = 300

function try_transmit(packet):
    for attempt in 0..2:
        if radio.cad_check():  // channel busy
            backoff = min(LBT_BACKOFF_BASE_MS * 2^attempt, LBT_BACKOFF_MAX_MS)
            sleep_ms(backoff + random(0, backoff))
            continue
        break  // channel clear
    
    // Transmit regardless after max attempts (anti-starvation)
    radio.transmit(packet)
    airtime_budget.debit(packet.airtime_ms)
```

LBT applies to **all** packet types at the `transmit_packet` layer, providing collision avoidance for beacons, data, routing, ACKs, and delivery receipts alike.

### 3.4 Broadcast Delivery Receipt Collision Avoidance

When multiple nodes receive a broadcast message, each must send a delivery receipt back to the sender. Without coordination, these receipts collide on the shared LoRa channel — LoRa has no built-in CSMA/CA, so simultaneous transmissions destroy each other.

Bramble uses a three-layer approach to prevent receipt collisions:

**Layer 1 — Slotted response timing:** Each recipient is assigned a deterministic time slot based on a hash of its address and the original packet ID:

```
SLOT_BUCKETS    = 32
SLOT_SPACING_MS = 200
SLOT_BASE_MS    = 200

slot = (local_addr XOR original_packet_id) % SLOT_BUCKETS
delay = SLOT_BASE_MS + (slot × SLOT_SPACING_MS) + random(0, 139)
```

This spreads receipt transmissions across a ~6.4-second window. With 32 buckets and typical mesh sizes (5–20 nodes), birthday-problem collisions are rare.

**Layer 2 — LBT (§3.3):** Each receipt transmission passes through the CAD check, providing a second chance to detect and avoid an in-progress transmission.

**Layer 3 — Exponential retry backoff:** Each receipt is transmitted up to 3 times with increasing randomized delays between attempts:

```
RECEIPT_RETRY_COUNT = 3

for attempt in 0..2:
    transmit(receipt)  // goes through LBT
    if attempt < 2:
        base  = 500 + (attempt × 700)    // 500ms, 1200ms
        range = 500 + (attempt × 400)    // 500ms, 900ms
        sleep_ms(base + random(0, range)) // 500–999ms, 1200–2099ms
```

The combination of wide slot spacing, hardware channel sensing, and aggressive retries achieves near-100% delivery receipt rates in meshes of 5+ nodes.

---

## 4. Packet Format

### 4.1 Design Constraints

- Maximum LoRa payload: 222 bytes (SX1262 with explicit header)
- Bramble header must be compact — every header byte costs ~4.3ms airtime at SF10/125kHz
- All multi-byte integers are **big-endian** (network byte order)
- Node addresses are 4 bytes (truncated hash of public key)

### 4.2 Common Packet Header (12 bytes)

Every Bramble packet starts with this header:

```
Offset  Size  Field           Description
──────  ────  ─────           ───────────
0       1     version         Protocol version (0x01)
1       1     packet_type     Packet type enum (see §4.3)
2       1     flags           Bitfield (see below)
3       1     hop_limit       TTL, decremented each hop (max 8)
4       4     dest_addr       Destination node address (0xFFFFFFFF = broadcast)
8       4     packet_id       Unique packet ID (sender-local counter + random bits)
──────────────────────────────────────────────────────────
Total: 12 bytes
```

> **Firmware reality (wire v2).** `version` is `2`, not `0x01` as shown above (`BRAMBLE_VERSION` in `components/packet/include/packet.h`). The RX path drops any packet whose header version does not match before any type-specific parsing (`bramble_header_is_supported_version`, checked in `mesh_process_rx_packet`). See §4.25 for the full wire v2 change inventory.

**Flags byte (bit fields):**

```
Bit 7   Bit 6   Bit 5   Bit 4   Bit 3    Bit 2    Bit 1   Bit 0
─────   ─────   ─────   ─────   ─────    ─────    ─────   ─────
TIER1   TIER0   ACK_REQ RECEIPT CHANNEL  ENCRYP   FRAG1   FRAG0
```

- `TIER[1:0]`: Message tier — 00=Broadcast, 01=Normal, 10=Critical, 11=Reserved
- `ACK_REQ`: Sender requests ACK from destination
- `RECEIPT`: Sender requests delivery receipt with relay path
- `CHANNEL`: 1=channel (group) message, 0=direct message
- `ENCRYP`: 1=payload encrypted, 0=plaintext (only for beacons/control). An earlier revision repurposed this bit as `HEADER_FLAG_EMERGENCY` for the (since removed, section 4.19) emergency packets; that collision is gone with them, and the bit means encryption only.
- `FRAG[1:0]`: Fragment indicator — 00=not fragmented, 01=first fragment, 10=middle fragment, 11=last fragment

> **Firmware reality (wire v2).** Bits 7:6 are no longer `TIER[1:0]`. Tier moved into the LOCATION ciphertext (§4.25 item 4), freeing those two bits: bit 7 is `FLAG_RESERVED_HIGH` (unused), bit 6 is `FLAG_EMERGENCY` (reserved for a future origin-set, AAD-bound emergency facility, not implemented today). Bits 5 through 0 are unchanged. See §4.25 item 2.

### 4.3 Packet Types

```
Value  Name               Description
─────  ────               ───────────
0x01   ACK                Acknowledgment
0x02   ROUTE_REQUEST      Route discovery query
0x03   ROUTE_REPLY        Route discovery response
0x04   ROUTE_ERROR        Route broken notification
0x05   BEACON             Periodic neighbor advertisement
0x06   KEY_EXCHANGE       X25519 DH key exchange
0x07   DELIVERY_RECEIPT   Delivery confirmation with relay path
0x08   (retired)          Was CONGESTION; removed unshipped (see §4.12)
0x09   (retired)          Was TIME_SYNC; removed unshipped (see §4.13)
0x0A   DATA               Application data (text, telemetry, etc.)
0x0B   STORE_REQUEST      Request mailbox node to store message (see §4.15)
0x0C   STORE_ACK          Mailbox storage acknowledgment (see §4.16)
0x0D   MAILBOX_DELIVERY   Delivery of stored message from mailbox (see §4.17)
0x0E   MAILBOX_QUERY      Query mailbox for pending messages (see §4.18)
0x0F   (retired)          Was EMERGENCY; removed unshipped (see §4.19)
0x10   (retired)          Was EMERGENCY_CANCEL; removed unshipped (see §4.20)
0x11   (retired)          Was CODED; removed unshipped (see §4.21)
0x12   PKT_TYPE_PROBE    Broadcast delivery probe (see §4.22)
0x13   PKT_TYPE_PROBE_ACK      Broadcast probe acknowledgment (see §4.23)
0x14   LOCATION           Location sharing packet
```

> **Firmware reality.** Of the types above, the mesh RX path (`mesh_process_rx_packet` in `main/mesh_task.c`) sends and handles only `ACK`, `ROUTE_REQUEST`, `ROUTE_REPLY`, `ROUTE_ERROR`, `BEACON`, `DELIVERY_RECEIPT`, `DATA`, `LOCATION`, `PROBE`, and `PROBE_ACK`. `KEY_EXCHANGE` and the mailbox types (`0x0B`-`0x0E`) are defined in `components/packet/include/packet.h` and unit-tested at component level but are never transmitted or dispatched today. Mailbox store-and-forward ships without its dedicated packet types: relays store undeliverable `DATA` and flush on the destination's beacon. The retired codes (`0x08`, `0x09`, `0x0F`-`0x11`) belonged to machinery that was removed unshipped; they stay reserved so a future wire version can reassign them deliberately.

### 4.4 DATA Packet

Used for all application-layer messages (text, telemetry, position, etc.).

**Direct Message DATA (encrypted end-to-end):**

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       12    header           Common header (packet_type=0x01, ENCRYP=1, CHANNEL=0)
12      4     src_addr         Sender node address
16      4     next_hop         Next hop address (for route-based forwarding)
20      1     app_type         Application type (0x01=text, 0x02=position, 0x03=telemetry, 0x04=file_chunk)
21      1     payload_len      Length of encrypted payload (0–185)
22      12    nonce            AES-256-GCM nonce (96-bit)
34      N     ciphertext       Encrypted payload (N = payload_len - 16)
34+N    16    auth_tag         AES-256-GCM authentication tag
──────────────────────────────────────────────────────────
Total: 50 + plaintext_len bytes (max 222)
Max plaintext payload: 172 bytes
```

**Channel Message DATA (channel PSK encrypted):**

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       12    header           Common header (packet_type=0x01, ENCRYP=1, CHANNEL=1)
12      4     src_addr         Sender node address (visible to channel members only after decryption)
16      4     next_hop         Next hop address (0xFFFFFFFF for broadcast-routed)
20      1     payload_len      Length of encrypted payload (0–184)
21      12    nonce            AES-256-GCM nonce
33      N     ciphertext       Encrypted payload (contains channel_id, epoch, app_type, src_addr, data)
33+N    16    auth_tag         AES-256-GCM authentication tag
──────────────────────────────────────────────────────────
Total: 49 + plaintext_len bytes (max 222)
Max plaintext payload: 173 bytes

Encrypted payload structure (inside ciphertext):
    Byte 0:     channel_id      Channel index (0–15)
    Byte 1–2:   epoch           Channel key epoch counter (see §5.3)
    Byte 3:     app_type        Application type
    Byte 4–7:   src_addr        Sender node address
    Byte 8+:    data            Application payload
```

Note: For channel messages, `src_addr` at offset 12 in the wire header is set to `0x00000000`. The `channel_id` is NOT present in the plaintext header — it is inside the encrypted payload. Receivers attempt trial decryption with each of their channel keys (max 16 attempts, ~1ms total on ESP32-S3 with hardware AES acceleration). This prevents non-members from identifying which channel a message belongs to. The actual source address is also encrypted inside the ciphertext alongside the payload.

### 4.5 ACK Packet

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       12    header           Common header (packet_type=0x02)
12      4     src_addr         ACK sender (original destination)
16      4     ack_packet_id    Packet ID being acknowledged
20      1     ack_flags        0x01=success, 0x02=buffer_full, 0x04=route_unknown
21      1     rssi_at_dest     RSSI at which the acked packet was received (signed, +128 offset)
──────────────────────────────────────────────────────────
Total: 22 bytes
```

ACKs are routed back along the reverse path. They are small and high-priority. The `rssi_at_dest` field lets the sender gauge link quality to the destination.

> **Firmware reality (wire v2).** The current `bramble_ack_t` also carries a `hop_count` and a variable-length `relay_path[]` (for critical-tier ACKs) beyond the fields shown above, and as of this batch a fixed-offset `auth_hmac[8]` sits immediately before `relay_path`, so the base size is 31 bytes (up to 63 with a full 8-hop path), not 22. See §4.25 item 6 and `components/packet/include/packet.h` for the current layout.

### 4.6 ROUTE_REQUEST Packet

Privacy-preserving route discovery. Intermediate nodes cannot determine who initiated the request — only the destination can decrypt the source identity.

```
Offset  Size  Field                Description
──────  ────  ─────                ───────────
0       12    header               Common header (packet_type=0x03, dest_addr=target)
12      4     query_id             Ephemeral random ID for this discovery (NOT derived from source)
16      4     encrypted_source     Source address encrypted with destination's public key (see §6.2)
20      1     hop_count            Hops traversed so far (starts 0, incremented by relays)
21      1     metric               Cumulative path quality metric (starts 255, decremented per hop)
22      4     prev_hop             Address of previous relay (set by each forwarder)
26      4     rreq_salt            Per-RREQ random salt (prevents temporal correlation of repeated discoveries)
──────────────────────────────────────────────────────────
Total: 30 bytes
```

The `encrypted_source` field is the 4-byte source address encrypted under a lightweight scheme using the destination's public key (see §5.4 for the encryption method). The `rreq_salt` is included in the OTP derivation, ensuring that repeated route discoveries for the same destination produce different `encrypted_source` values and cannot be correlated by observers. Intermediate nodes see only random-looking bytes. The destination decrypts to learn who is looking for it.

### 4.7 ROUTE_REPLY Packet

Unicast back along the reverse path built during ROUTE_REQUEST propagation.

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       12    header           Common header (packet_type=0x04)
12      4     query_id         Matching query_id from the ROUTE_REQUEST
16      4     src_addr         Destination of the original RREQ (i.e. the replier)
20      4     next_hop         Next hop back toward originator
24      1     hop_count        Total hops in discovered route
25      1     route_metric     End-to-end path quality metric
26      8     auth_hmac        Truncated HMAC-SHA256 (8 bytes) over bytes 0–25, keyed with
                               static DH shared secret between RREQ originator and destination.
                               Set to 0x0000000000000000 for first-contact (no shared secret);
                               route marked "unverified" until KEY_EXCHANGE completes.
──────────────────────────────────────────────────────────
Total: 34 bytes
```

**RREQ Flags (encoded in bits of the `flags` byte in the common header):**

- Bit 5 (reserved in common header): `OPEN_SOURCE` — when set, `encrypted_source` contains the plaintext source address (not encrypted). Used for first-contact discovery when the destination's public key is unknown. See §5.4 for details.

> **Firmware reality (wire v2).** `auth_hmac` is no longer dead or zeroed. It authenticates `query_id || src_addr || hop_count || route_metric` with a network-key HMAC (label `"bramble-rrep-v2"`), excluding `next_hop` and `header.dest_addr` (the two fields `rrep_forward` legitimately rewrites at each relay). The static-DH-shared-secret keying scheme described above was never implemented and does not reflect any shipped version of the code. `RREP_SIZE` is unchanged at 34 bytes (the field already existed on the wire, unused). See §4.25 item 7.

### 4.8 ROUTE_ERROR Packet

Sent when a forwarding node detects a broken link (no ACK from next hop after max retries).

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       12    header           Common header (packet_type=0x05)
12      4     reporter_addr    Node that detected the break
16      4     broken_dest      Destination that is now unreachable
20      4     broken_next_hop  The next-hop that failed
──────────────────────────────────────────────────────────
Total: 24 bytes
```

> **Firmware reality (wire v2).** `RERR_SIZE` is now 32 bytes (was 24). A trailing `auth_hmac[8]` authenticates `broken_dest || broken_next_hop` (the two origin-stable fields), excluding `reporter_addr` and `packet_id`, which every forwarder rewrites when it re-originates a RERR. Verified before any teardown action. See §4.25 item 5.

### 4.9 BEACON Packet

Periodic advertisement. Broadcast, unencrypted (beacons contain no sensitive content).

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       12    header           Common header (packet_type=0x06, dest=0xFFFFFFFF, ENCRYP=0)
12      4     src_addr         Beacon sender's address
16      4     node_pubkey_hash First 4 bytes of SHA-256(X25519_public_key) — for key lookup
20      2     uptime_min       Node uptime in minutes (0–65535, ~45 days max)
22      1     battery_pct      Battery percentage (0–100, 0xFF=unknown/plugged in)
23      1     tx_queue_depth   Current TX queue depth (0–16, congestion signal)
24      1     neighbor_count   Number of known neighbors
25      1     flags            Beacon flags bitfield (see below)
26      4     network_time     Current network time estimate (epoch seconds, lower 32 bits)
30      2     time_confidence  Confidence in time estimate (ms uncertainty, 0=GPS-synced)
32      4     auth_hmac        Truncated HMAC-SHA256 (4 bytes) over bytes 0–31. Keyed with
                               pairwise DM session key if one exists with the receiver. Set to
                               0x00000000 if no session key exists. Receivers verify beacons from
                               known peers; unknown peers' beacons are flagged lower-trust.
──────────────────────────────────────────────────────────
Total: 36 bytes (base)
```

**Beacon flags byte (bit fields):**

```
Bit 7   Bit 6   Bit 5   Bit 4   Bit 3    Bit 2        Bit 1         Bit 0
─────   ─────   ─────   ─────   ─────    ─────        ─────         ─────
RSVD    RSVD    RSVD    RSVD    RSVD     ACCEPT_DM    PROBE_ACK     MAILBOX
```

- `MAILBOX` (0x01): Node is willing to store messages for offline peers (see §4.15–4.18)
- `PROBE_ACK` (0x02): Node will respond to broadcast probes (see §4.22–4.23)
- `ACCEPT_DM` (0x04): Node is accepting direct messages (legacy: `accepting_dms`)
- Bits 3–7: Reserved for future use

**Route Advertisement Extension:** Removed unshipped. The passive route-learning extension (up to 4 route ads appended to the beacon) was deleted without ever being placed on the wire; beacons carry the optional node name after the fixed fields instead.

> **Firmware reality (wire v2).** The HMAC key is now derived from the network key when one is provisioned (`HKDF-SHA256(salt="bramble-beacon-v2", ikm=network_key)`), a distinct subkey rather than a channel-PSK-derived key; the public-PSK-derived fallback remains, logged explicitly as integrity-only, when unprovisioned. Verification (`beacon_verify_hmac`) is constant-time (XOR-accumulate, no early exit), not a fast-exit compare. See §4.25 item 8.

### 4.10 KEY_EXCHANGE Packet

X25519 Diffie-Hellman key exchange for establishing DM encryption keys.

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       12    header           Common header (packet_type=0x07)
12      4     src_addr         Initiator's address
16      32    ephemeral_pubkey X25519 ephemeral public key
48      4     key_id           Key exchange session ID (random)
52      1     ke_type          0x01=initiate, 0x02=respond, 0x03=confirm
53      32    long_term_pubkey Sender's long-term X25519 public key (allows recipient to
                               derive static DH shared secret without prior beacon exchange)
85      16    auth_tag         HMAC-SHA256 truncated to 128 bits (over bytes 0–84, keyed with long-term shared secret if rekeying, or zeros if first exchange)
──────────────────────────────────────────────────────────
Total: 101 bytes
```

> **Firmware reality (wire v2).** As of this batch `PKT_TYPE_KEY_EXCHANGE` (0x06) is formally retired from the wire: it is not sent or dispatched (as §4.3 already notes it never was), and DM handshakes now travel inside `PKT_TYPE_DATA` envelopes with `app_type = APP_TYPE_KE` instead, using per-peer AES-256-GCM session keys derived from a quad-DH X25519 exchange plus a 7-digit SAS. `bramble_key_exchange_t` remains in `packet.h`/`packet.c` as the inner-payload layout for those handshake messages; the standalone `0x06` packet type never appears on the wire. See §4.25 item 3 and `docs/SECURITY-MODEL.md`.

### 4.11 DELIVERY_RECEIPT Packet

Returned to sender with relay path information. This is how senders passively build routing intelligence.

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       12    header           Common header (packet_type=0x08)
12      4     src_addr         Receipt generator (original destination)
16      4     orig_packet_id   Packet ID of the delivered message
20      1     hop_count        Number of hops in the delivery path
21      1     total_latency    Estimated one-way latency in 100ms units (0–25.5s)
22      N×4   relay_path       Array of node addresses in relay order (N = hop_count, max 8)
──────────────────────────────────────────────────────────
Total: 22 + (hop_count × 4) bytes (max 54 bytes at 8 hops)
```

Each relay node appends its own address to the relay_path as it forwards the receipt back toward the original sender. The sender receives the complete path and can update its routing intelligence.

**Relay path restriction:** Delivery receipts with relay paths (the full DELIVERY_RECEIPT packet above) are only generated for **Critical tier** messages. Normal tier messages receive simple ACKs only (§4.5) — no relay path data. The `RECEIPT` flag in the common header flags byte should only be set for Critical tier. This limits exposure of relay path data, which is visible to nodes on the return route. This is an acceptable tradeoff since those nodes already know they are relays for that specific delivery.

> **Firmware reality (wire v2).** `DELIVERY_RECEIPT_MIN_SIZE`/`MAX_SIZE` are now 30/62 bytes (were 22/54). A fixed-offset `auth_hmac[8]` sits immediately before `relay_path`, authenticating `src_addr || orig_packet_id`, so a verifier never has to trust the unauthenticated `hop_count` to locate the tag. Excludes `relay_path`/`hop_count`/`header.hop_limit`, which every relay hop mutates. See §4.25 item 6.

### 4.12 CONGESTION Packet

Removed unshipped. The congestion-notification packet and its serializers were deleted without ever being transmitted or handled; airtime admission is enforced locally by the budget-gated TX path (section 8.2.1) instead of by neighbor congestion signaling.

### 4.13 TIME_SYNC Packet

Removed unshipped. Time synchronization rides the beacon (`network_time`, `time_confidence`, stratum; see section 9); the dedicated TIME_SYNC packet and its serializers were deleted without ever being transmitted or handled.

### 4.14 Fragmentation

For payloads exceeding the single-packet maximum (172 bytes for DM, 171 for channel), Bramble supports fragmentation using the FRAG bits in the flags byte.

Fragment header (appended after the standard DATA header, before payload):

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       1     frag_index       Fragment index (0–based)
1       1     frag_total       Total fragments in this message
2       2     message_id       Reassembly message ID (shared across fragments)
──────────────────────────────────────────────────────────
Total: 4 bytes (reduces per-fragment payload by 4 bytes)
```

**Fragment-then-encrypt ordering:** Fragmentation happens BEFORE encryption. The plaintext message is split into fragments, and each fragment is independently encrypted with its own nonce and 16-byte AES-256-GCM auth tag. This means:

- **Per-fragment auth tag overhead:** 16 bytes × N fragments (vs 16 bytes total for encrypt-then-fragment)
- **Max per-fragment plaintext:** 154 bytes (168 bytes minus 4 byte frag header, with nonce and auth tag in the DATA wrapper)
- **Max reassembled plaintext:** 4 fragments × 154 bytes = **616 bytes**. This is a hard protocol limit. Applications requiring more must implement their own chunking.
- **Each fragment is independently verifiable** — receivers can authenticate and discard forged fragments immediately without buffering the entire message. This prevents reassembly buffer exhaustion attacks.
- **Clean layering:** The fragment layer splits plaintext, then each fragment passes through normal DATA encryption independently. No special crypto handling at the fragment layer.

Fragment reassembly buffer: max 4 concurrent reassemblies × 4 fragments × 154 bytes = **2,464 bytes**.

```
Reassembly timeout: 30 seconds (all fragments must arrive within this window)
Duplicate fragment: silently dropped (after auth tag verification)
Out-of-order: fragments decrypted individually, plaintext stored in bitmap; reassembled when all present
Missing fragment after timeout: entire message dropped, NACK sent if tier ≥ Normal
Invalid auth tag: fragment dropped immediately (no buffer allocation)
```

### 4.15 STORE_REQUEST Packet

Requests a mailbox node to store a message for an offline destination. Used when route discovery to the destination fails but a mailbox-capable neighbor is available.

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       12    header           Common header (packet_type=0x0B)
12      4     src_addr         Original message source address
16      4     orig_dest_addr   Intended destination (the offline node)
20      4     orig_packet_id   Original packet ID (for dedup at destination)
24      4     ttl_s            Requested time-to-live in seconds (capped by mailbox)
28      2     payload_len      Length of enclosed encrypted DATA packet
30      N     payload          The original encrypted DATA packet (max 200 bytes)
30+N    4     auth_hmac        HMAC-SHA256 truncated to 4 bytes over bytes 0–(29+N)
──────────────────────────────────────────────────────────
Total: 34 + payload_len bytes (max 234 bytes)
```

The `auth_hmac` is computed using a pairwise session key with the mailbox node, preventing unauthorized nodes from stuffing the mailbox.

**Buffer limits:**
- `MAILBOX_MAX_ENTRIES`: 32 total stored messages
- `MAILBOX_MAX_PER_DEST`: 8 messages per destination address
- `MAILBOX_MAX_PER_SOURCE`: 8 messages per source address
- `MAILBOX_MAX_PAYLOAD`: 200 bytes
- `MAILBOX_TTL_MS`: 24 hours (86,400,000 ms) maximum retention

### 4.16 STORE_ACK Packet

Acknowledgment that a mailbox node has accepted and stored a message.

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       12    header           Common header (packet_type=0x0C)
12      4     src_addr         Mailbox node's address
16      4     orig_packet_id   Packet ID being acknowledged as stored
20      1     status           0x01=stored, 0x02=rejected_full, 0x03=rejected_ttl
21      4     expires_at       Expiration timestamp (epoch seconds) if stored
──────────────────────────────────────────────────────────
Total: 25 bytes
```

### 4.17 MAILBOX_DELIVERY Packet

Delivery of a stored message when the destination comes back online (detected via beacon).

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       12    header           Common header (packet_type=0x0D)
12      4     mailbox_addr     Mailbox node delivering the message
16      4     orig_src_addr    Original message source
20      4     orig_packet_id   Original packet ID
24      4     stored_at        Timestamp when message was stored (epoch seconds)
28      2     payload_len      Length of enclosed DATA packet
30      N     payload          The original encrypted DATA packet
──────────────────────────────────────────────────────────
Total: 30 + payload_len bytes
```

Delivery is paced at one message per beacon interval (~60s) to avoid flooding a newly-returned node. Messages are delivered in FIFO order (oldest first).

### 4.18 MAILBOX_QUERY Packet

A node queries a mailbox for pending messages (used when destination proactively polls rather than waiting for beacon-triggered delivery).

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       12    header           Common header (packet_type=0x0E)
12      4     src_addr         Querying node's address
16      4     mailbox_addr     Target mailbox node
20      4     auth_hmac        HMAC-SHA256 truncated to 4 bytes (proves identity)
──────────────────────────────────────────────────────────
Total: 24 bytes
```

The mailbox responds with MAILBOX_DELIVERY packets for any stored messages, or an empty STORE_ACK with status=0x00 if no messages are pending.

### 4.19 EMERGENCY Packet

Removed unshipped. The emergency-beacon state machine and packet were deleted without ever being transmitted or handled, and the `HEADER_FLAG_EMERGENCY` define collided with `FLAG_ENCRYPT` (both `0x04`), so the flag could never have been used on the v1 wire. A redesigned emergency facility (origin-set, AAD-bound flag) is specified for the next wire version in the cryptographic design RFC.

### 4.20 EMERGENCY_CANCEL Packet

Removed unshipped, together with section 4.19.

### 4.21 CODED Packet

Removed unshipped. The XOR network-coding relay optimization (FEC-style coded packets) was deleted without a coded packet ever being transmitted; relays forward packets uncoded.

### 4.22 PKT_TYPE_PROBE Packet

Probes broadcast reachability to discover network topology. Used for network health visualization and delivery confirmation.

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       1     version          Protocol version (0x01)
1       1     type             Packet type (0x12)
2       1     flags            Probe flags (see below)
3       1     hop_limit        TTL (default 8, the max route depth)
4       4     src_addr         Probe originator's address
8       4     probe_id         Unique probe ID
──────────────────────────────────────────────────────────
Total: 12 bytes
```

**Probe flags:**
- `PROBE_FLAG_INCLUDE_RSSI` (0x01): Request responders include RSSI in ACK
- `PROBE_FLAG_INCLUDE_PATH` (0x02): Request relay path in ACK
- `PROBE_FLAG_SILENT` (0x04): Nodes should not acknowledge (observe-only)

**Rate limiting:**
- `PROBE_RATE_LIMIT_TOKENS`: 3 (max burst)
- `PROBE_RATE_LIMIT_REFILL_MS`: 60,000 (1 token per minute)
- `PROBE_ACK_COOLDOWN_MS`: 10,000 (10s between ACKs to same probe)

### 4.23 PKT_TYPE_PROBE_ACK Packet

Acknowledgment of a broadcast probe. Provides reachability and signal quality information.

```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       1     version          Protocol version (0x01)
1       1     type             Packet type (0x13)
2       1     flags            Same flags as probe (indicates which optional fields present)
3       1     hop_count        Number of hops from probe originator
4       4     src_addr         ACK sender's address
8       4     probe_id         Probe ID being acknowledged
12      1     rssi             RSSI at which probe was received (optional, if INCLUDE_RSSI)
13      N×4   path             Relay path addresses (optional, if INCLUDE_PATH, N = hop_count)
──────────────────────────────────────────────────────────
Total: 12–13 bytes minimum (depending on flags), variable with path
```

**Response collection window:** 30 seconds (`PROBE_COLLECTION_WINDOW_MS`)

**ACK jitter:** 100–2000ms random delay before responding to reduce collisions

### 4.24 Private Location Sharing

Location updates are sent as encrypted DATA packets (type=0x0A) with a location-specific `app_type` in the payload. Three privacy tiers are supported:

**LOCATION_FULL (17 bytes):**
```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       4     latitude_e7      Latitude in degrees × 10⁷
4       4     longitude_e7     Longitude in degrees × 10⁷
8       2     altitude_m       Altitude in meters (signed)
10      1     accuracy_m       Horizontal accuracy (0–255 meters)
11      1     speed_kmh        Speed (0–255 km/h)
12      1     heading_deg2     Heading / 2 (0–179 maps to 0–358°)
13      4     timestamp        Position timestamp (epoch seconds)
──────────────────────────────────────────────────────────
Total: 17 bytes
```

**LOCATION_COARSE (5 bytes):**
```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       2     grid_lat         Quantized latitude (~1 km resolution)
2       2     grid_lon         Quantized longitude (~1 km resolution)
4       1     ts_low           Low byte of timestamp
──────────────────────────────────────────────────────────
Total: 5 bytes
```

**LOCATION_PRESENCE (1 byte):**
```
Offset  Size  Field            Description
──────  ────  ─────            ───────────
0       1     status           Bit 0: online/offline
──────────────────────────────────────────────────────────
Total: 1 byte
```

**Privacy tiers (per-contact configuration):**
| Tier | Constant | Data Shared | Resolution |
|------|----------|-------------|------------|
| Full | `LOCATION_TIER_FULL` (0) | lat, lon, alt, speed, heading, accuracy | ~1m (GPS) |
| Coarse | `LOCATION_TIER_COARSE` (1) | Grid square | ~1 km |
| Presence | `LOCATION_TIER_PRESENCE` (2) | Online/offline only | None |

**Update triggers:**
- Time-based: default 5-minute interval (`LOCATION_DEFAULT_INTERVAL_MS`)
- Distance-based: default 100m threshold (`LOCATION_MIN_DISTANCE_M`)

**Cache:**
- `LOCATION_MAX_CONTACTS`: 16 peers
- `LOCATION_CACHE_TTL_MS`: 1 hour

### 4.25 Wire Version 2 (Firmware Reality)

`BRAMBLE_VERSION` is `2` (`components/packet/include/packet.h`). The RX path drops any packet whose header version does not match before body parsing (`bramble_header_is_supported_version`, checked in `mesh_process_rx_packet` before dedup or handler dispatch). There is no v1/v2 compatibility shim: a v1 node and a v2 node cannot talk to each other, and none was attempted.

This section is the wire v2 change inventory for `feat/wire-format-security-batch`. It supersedes the packet tables above (§4.2 through §4.11) wherever they conflict; the per-section notes above point back here.

1. **Version bump and RX gate.** `BRAMBLE_VERSION 1 -> 2`. Non-v2 packets are rejected before any type-specific parsing.
2. **Flags byte redesign (tier out of flags).** The v1 `TIER[1:0]` bits (7:6) are gone; tier now travels inside the LOCATION ciphertext (item 4 below), not the cleartext header. Bits 7:6 are `FLAG_RESERVED_HIGH` (0x80, unused) and `FLAG_EMERGENCY` (0x40, reserved for a future origin-set, AAD-bound emergency facility, not implemented). Note: DES-9, the historical collision between an emergency flag and `FLAG_ENCRYPT` (both `0x04`), was already resolved before this batch when the unshipped emergency packet machinery was deleted (§4.19, §4.20); this flag redesign is a distinct, later cleanup, not the DES-9 fix itself.
3. **`PKT_TYPE_KEY_EXCHANGE` (0x06) retired from the wire.** DM handshakes now travel inside `PKT_TYPE_DATA` envelopes with `app_type = APP_TYPE_KE`, using per-peer AES-256-GCM session keys derived from a quad-DH X25519 exchange plus a 7-digit SAS. `0x06` never appears on the wire again.
4. **DATA and LOCATION are AEAD with a bound header.** Both use AES-256-GCM with a 12-byte nonce derived from a node-global, NVS-persisted 48-bit deterministic counter (`components/nonce_counter`), not the random-nonce scheme this document described previously. The associated data binds the serialized 12-byte header (with `hop_limit` zeroed, the one field a relay legitimately mutates) plus, for DATA, the sender's `src_addr`. LOCATION packets are now always encrypted; the sharing tier moves from the old cleartext header bits into byte 0 of the authenticated plaintext, and every LOCATION ciphertext pads to one canonical size regardless of tier, so ciphertext length no longer leaks which tier was chosen. Post-decrypt replay protection is enforced per-sender on `(src_addr, nonce_counter_extract(nonce))`.
5. **ROUTE_ERROR (RERR) plus 8 bytes.** `RERR_SIZE 24 -> 32`. A trailing `auth_hmac[8]` authenticates `broken_dest || broken_next_hop` with a network-key HMAC (label `"bramble-rerr-v2"`); `reporter_addr` and `packet_id` are excluded because each forwarder rewrites both on re-origination. Verified before any teardown action.
6. **ACK plus 8 bytes, DELIVERY_RECEIPT plus 8 bytes.** `ACK_BASE_SIZE 23 -> 31` (`ACK_MAX_SIZE` 55 -> 63); `DELIVERY_RECEIPT_MIN_SIZE 22 -> 30` (`DELIVERY_RECEIPT_MAX_SIZE` 54 -> 62). Both structs gain `auth_hmac[8]`, authenticating `src_addr || ack_packet_id` (label `"bramble-ack-v2"`) and `src_addr || orig_packet_id` (label `"bramble-receipt-v2"`) respectively. The field sits immediately before the variable-length `relay_path`, at a fixed offset independent of `hop_count`, so a verifier never has to trust the attacker-controlled, unauthenticated `hop_count` to locate the tag. Excludes `relay_path`, `hop_count`, and `hop_limit`, which every relay hop legitimately mutates.
7. **ROUTE_REPLY (RREP) `auth_hmac` is now real.** `RREP_SIZE` is unchanged at 34 bytes (the field already existed on the wire, zeroed and ignored). It now authenticates `query_id || src_addr || hop_count || route_metric` with label `"bramble-rrep-v2"`, excluding `next_hop` and `header.dest_addr`, the two fields `rrep_forward` legitimately rewrites at each relay.
8. **BEACON HMAC re-keyed.** When a network key is provisioned, the beacon HMAC key is `HKDF-SHA256(salt="bramble-beacon-v2", ikm=network_key)`, a distinct subkey rather than a channel-PSK-derived key; the public-PSK-derived fallback remains, logged as integrity-only, when unprovisioned. Verification is now constant-time (XOR-accumulate, no early exit). Operational note: `bramble.setNetworkKey` takes effect immediately for RREP, RERR, ACK, and receipt MACs (each call reads the current key live); the beacon key used to be cached at init and require a reboot to pick up a runtime-provisioned key, but the RPC handler now also calls `mesh_rederive_beacon_key`, so beacons re-key live too and that gap is resolved.
9. **Minimal network key provider added (`components/network_key`).** NVS-backed (namespace `bramble_netkey`), settable via the authenticated `bramble.setNetworkKey` RPC (gated the same way as `bramble.setAuthToken`: absence from the dispatcher's unauthenticated-method allowlist, not a per-handler check). Unprovisioned nodes fall back to a key derived from the public, compile-time `BRAMBLE_PUBLIC_CHANNEL_PSK`, so items 5 through 8 above are wire format and verification logic only until a real key is provisioned. Generation, out-of-band distribution (the `bramble://net/v1` share URI above), and fleet-convergence verification (`bramble.getNetworkKeyStatus` fingerprint) now ship in the webapp; key rotation UX does not. See `docs/SECURITY-MODEL.md` for why SEC-H1, SEC-H2, NEW-SEC-4, and NEW-SEC-8 still do not close: provisioning excludes non-members but does not stop a member from forging on another member's behalf, does not add replay protection to any of the five MACs, and does not close the NEW-SEC-4 bootstrap-quorum race, which needs per-node beacon identity.

### 4.26 Wire Version 3 (Firmware Reality)

`BRAMBLE_VERSION` is `3` (was `2`; `components/packet/include/packet.h`). The RX gate (`bramble_header_is_supported_version`, checked in `mesh_process_rx_packet` before dedup or handler dispatch) is a strict `==`, so this is a flag day: a v2 packet is rejected outright, not accepted as legacy-compatible, and no v2/v3 shim was attempted, same policy as the v1 -> v2 bump in §4.25.

This section is the wire v3 change inventory for the ws 1.3b control-plane freshness workstream. Only the five messages listed below change layout; DATA, LOCATION, RREQ, and PROBE are unchanged in layout but now carry `version = 3`.

1. **Version bump and RX gate.** `BRAMBLE_VERSION 2 -> 3`. Non-v3 packets are rejected before any type-specific parsing, same mechanism as item 1 in §4.25.
2. **All five control-plane MACs (RREP, RERR, ACK, delivery receipt, beacon) now bind a 48-bit origin sequence.** The sequence is drawn from the same node-global, NVS-persisted `nonce_counter` DATA/LOCATION already use for AEAD nonces (§4.25 item 4), reserve-ahead and fail-closed: if the counter is not ready, the message does not go out rather than shipping without freshness. Each receiver checks the authenticated `(signer, seq)` pair against a dedicated per-signer replay window (`components/replay_window`, a second instance separate from the DATA/LOCATION one, same 64-position sliding-window mechanism) and drops on any non-accept result. Under a provisioned network key, this closes the outsider-replay residual noted in §4.25 item 9 and `docs/SECURITY-MODEL.md`: a captured, genuinely-valid control-plane message can no longer be replayed. It does not close insider forgery (inherent to the shared symmetric key) or the NEW-SEC-4 bootstrap-quorum race (needs per-node beacon identity, staged separately as 1.3c); see `docs/SECURITY-MODEL.md` section 5 for the precise residual statement.
3. **RREP plus 6 bytes.** `RREP_SIZE 34 -> 40`. `seq[6]` sits immediately after `auth_hmac[8]` and joins the MAC's covered field set (`query_id || src_addr || hop_count || route_metric || seq`); origin-stable, drawn once by the destination that answers the RREQ, and carried through `rrep_forward` unchanged, same as the rest of the covered fields.
4. **RERR plus 6 bytes, and `reporter_addr` now MAC-covered.** `RERR_SIZE 32 -> 38`. `seq[6]` sits immediately after `auth_hmac[8]`. Unlike RREP's, RERR's seq is drawn fresh on every re-origination (`send_rerr`), matching `reporter_addr`, which moves into the MAC's covered set for the first time (`reporter_addr || broken_dest || broken_next_hop || seq`; was `broken_dest || broken_next_hop` only, per §4.25 item 5). Safe because every forwarder re-signs the whole struct with its own `reporter_addr` on re-origination, and it is what makes keying replay on `(reporter_addr, seq)` sound, since both halves are now authenticated and one `reporter_addr` maps to exactly one monotonic counter. `header.packet_id` remains the only excluded field.
5. **ACK and delivery receipt plus 6 bytes, seq at the same hop-independent offset as `auth_hmac`.** `ACK_BASE_SIZE 31 -> 37` (`ACK_MAX_SIZE` 63 -> 69); `DELIVERY_RECEIPT_MIN_SIZE 30 -> 36` (`DELIVERY_RECEIPT_MAX_SIZE` 62 -> 68). `seq[6]` sits immediately after `auth_hmac[8]` and before the variable-length `relay_path`, at the same fixed, `hop_count`-independent offset `auth_hmac` already used (§4.25 item 6), for the same reason: a verifier must never trust the attacker-controlled `hop_count` to locate a MAC-covered field. Both are origin-stable (`src_addr || ack_packet_id || seq`, `src_addr || orig_packet_id || seq`) and carried through forwarding unchanged.
6. **Beacon plus 6 bytes, seq INSIDE the HMAC-covered prefix.** `BEACON_SIZE 48 -> 54`. Unlike the other four, beacon's `seq[6]` sits BEFORE `auth_hmac[16]` (immediately after `time_confidence`), inside the fixed prefix `beacon_compute_hmac` already hashes (`prefix_len = BEACON_SIZE - sizeof(auth_hmac)`, always computed from the size macro, never hardcoded), so it is covered automatically without any change to the HMAC computation itself (§4.25 item 8). Single-hop, never forwarded, so there is no carry-through case to preserve, unlike RREP/ACK/receipt.

---

## 5. Node Identity & Key Management

### 5.1 Identity Generation

Each node generates a persistent identity on first boot:

```
function generate_identity():
    // Generate X25519 long-term key pair
    private_key = random_bytes(32)              // From ESP32 hardware RNG
    public_key = x25519_base_point_mult(private_key)
    
    // Derive 4-byte node address from public key
    addr_hash = sha256(public_key)
    node_addr = addr_hash[0..3]                 // First 4 bytes = node address
    
    // Store in NVS (non-volatile storage)
    nvs_write("bramble_privkey", private_key)   // 32 bytes
    nvs_write("bramble_pubkey", public_key)     // 32 bytes
    nvs_write("bramble_addr", node_addr)        // 4 bytes
    
    return (private_key, public_key, node_addr)
```

**Key Backup:** Node identity keys can be exported via BLE for backup purposes. Export requires physical button authorization (30-second window after button press). The exported blob is encrypted with AES-256-GCM using a user-provided passphrase, preventing extraction by BLE eavesdroppers or unauthorized apps.

**Address collision handling:** With 4-byte addresses (2³² space) and target networks of ~200 nodes, collision probability is ~0.0005% (birthday problem). If a collision is detected via beacon (two nodes claiming same address with different pubkeys), the node with the lexicographically smaller public key regenerates. Collision detection runs on every received beacon.

```
function on_beacon_received(beacon):
    if beacon.src_addr == my_addr AND beacon.node_pubkey_hash != my_pubkey_hash:
        if my_pubkey < beacon_sender_pubkey:  // Lexicographic comparison
            regenerate_identity()
            broadcast_beacon_immediately()
```

### 5.2 X25519 Key Exchange for DMs

> **Firmware reality.** This exchange is not implemented on the wire: `PKT_TYPE_KEY_EXCHANGE` is never sent and never handled, and DMs are encrypted with the shared channel key, readable by every holder of that key ([SECURITY-MODEL.md](SECURITY-MODEL.md), section 4).

When node A wants to send a DM to node B for the first time, they perform a key exchange:

**Step 1: A initiates** (requires knowing B's long-term public key, learned from beacons or cached)

```
function initiate_key_exchange(dest_addr, dest_pubkey):
    // Generate ephemeral key pair
    eph_private = random_bytes(32)
    eph_public = x25519_base_point_mult(eph_private)
    
    // Compute shared secret using both static and ephemeral keys
    // Double-DH: eph_A × static_B  and  static_A × static_B
    ss1 = x25519(eph_private, dest_pubkey)       // Ephemeral-static
    ss2 = x25519(my_private_key, dest_pubkey)    // Static-static
    
    // Derive session key using HKDF-SHA256
    ikm = ss1 || ss2                              // 64 bytes
    session_key = hkdf_sha256(
        salt = "bramble-dm-v1",
        ikm = ikm,
        info = min(my_addr, dest_addr) || max(my_addr, dest_addr),
        length = 32                               // AES-256 key
    )
    
    key_id = random_bytes(4)
    
    // Cache the session key
    key_cache.store(dest_addr, session_key, key_id, expiry=86400)  // 24hr expiry
    
    // Send KEY_EXCHANGE initiate packet
    send_key_exchange(dest_addr, eph_public, key_id, type=INITIATE, auth_tag=zeros(16))
    
    return key_id
```

**Step 2: B responds**

```
function handle_key_exchange_initiate(pkt):
    // Compute the same shared secret
    ss1 = x25519(my_private_key, pkt.ephemeral_pubkey)   // My static × their ephemeral
    ss2 = x25519(my_private_key, lookup_pubkey(pkt.src_addr))  // Static-static
    
    ikm = ss1 || ss2
    session_key = hkdf_sha256(
        salt = "bramble-dm-v1",
        ikm = ikm,
        info = min(my_addr, pkt.src_addr) || max(my_addr, pkt.src_addr),
        length = 32
    )
    
    key_cache.store(pkt.src_addr, session_key, pkt.key_id, expiry=86400)
    
    // Generate our own ephemeral for the response (for forward secrecy of future rekeys)
    resp_eph_private = random_bytes(32)
    resp_eph_public = x25519_base_point_mult(resp_eph_private)
    
    // Auth tag proves we derived the same key
    auth_tag = hmac_sha256(session_key, pkt.key_id || resp_eph_public)[0..15]
    
    send_key_exchange(pkt.src_addr, resp_eph_public, pkt.key_id, type=RESPOND, auth_tag=auth_tag)
```

**Step 3: A confirms**

```
function handle_key_exchange_response(pkt):
    session_key = key_cache.lookup(pkt.src_addr, pkt.key_id)
    expected_tag = hmac_sha256(session_key, pkt.key_id || pkt.ephemeral_pubkey)[0..15]
    
    if expected_tag != pkt.auth_tag:
        log("Key exchange auth failed for %s", pkt.src_addr)
        return ERROR
    
    // Key confirmed. Compute final forward-secret key incorporating both ephemerals.
    ss3 = x25519(my_cached_eph_private, pkt.ephemeral_pubkey)  // Both ephemerals
    final_key = hkdf_sha256(
        salt = "bramble-dm-v1-final",
        ikm = session_key || ss3,
        info = pkt.key_id,
        length = 32
    )
    
    key_cache.update(pkt.src_addr, final_key, pkt.key_id)
    
    // Send confirm
    confirm_tag = hmac_sha256(final_key, pkt.key_id || "confirm")[0..15]
    send_key_exchange(pkt.src_addr, zeros(32), pkt.key_id, type=CONFIRM, auth_tag=confirm_tag)
```

### 5.3 Channel PSK Management

Channels use pre-shared keys distributed out-of-band (QR code, manual entry, BLE provisioning).

```
Channel key derivation:
    channel_psk = user_provided_key (arbitrary length string)
    channel_key = hkdf_sha256(
        salt = "bramble-channel-v1",
        ikm = sha256(channel_psk),
        info = channel_id (1 byte),
        length = 32                    // AES-256 key
    )
    channel_id = sha256(channel_key)[0]  // First byte of hash = channel index (0–255, mod 16 for header)
```

Each node can belong to up to **16 channels** simultaneously (4-bit channel_id in encrypted payload). Channel keys are stored in NVS:

```
NVS layout:
    "bramble_ch_00_key" → 32 bytes (channel 0 AES-256 key, current epoch)
    "bramble_ch_00_name" → 32 bytes (channel 0 human-readable name)
    "bramble_ch_00_epoch" → 2 bytes (channel 0 current epoch counter)
    ...
    "bramble_ch_15_key" → 32 bytes
    "bramble_ch_15_name" → 32 bytes
    "bramble_ch_15_epoch" → 2 bytes
    
Total channel NVS: 16 × 66 = 1,056 bytes
```

#### Epoch-Based Channel Key Ratchet

Channel keys rotate via an epoch-based forward ratchet providing backward secrecy:

```
function advance_channel_epoch(channel_id):
    current_key = channel_keys[channel_id]
    current_epoch = channel_epochs[channel_id]
    
    // Derive next epoch key
    new_key = hkdf_sha256(
        salt = "bramble-channel-epoch",
        ikm = current_key,
        info = to_bytes_be16(current_epoch + 1),
        length = 32                    // AES-256 key
    )
    
    // Delete old key — provides backward secrecy
    channel_keys[channel_id] = new_key
    channel_epochs[channel_id] = current_epoch + 1
    channel_msg_counts[channel_id] = 0
    
    // Persist to NVS
    nvs_write("bramble_ch_%02d_key" % channel_id, new_key)
    nvs_write("bramble_ch_%02d_epoch" % channel_id, current_epoch + 1)
```

**Epoch advancement triggers:**
- Every **24 hours** (configurable via `channel_epoch_hours`)
- Every **256 messages** sent on the channel (configurable via `channel_epoch_messages`)
- Whichever comes first

**Catch-up for offline nodes:** A node offline for N epochs computes N HKDF iterations from its last known key to derive the current epoch key (~0.1ms per iteration on ESP32-S3 with SHA-256 HW acceleration). The 2-byte epoch counter in each channel message header (see §4.4) tells the receiver which epoch to target.

**Security properties:**
- **Backward secrecy:** Old epoch keys are deleted. An attacker who compromises the current key cannot decrypt past messages.
- **No forward secrecy:** An attacker who compromises the current key CAN derive all future keys via the same HKDF chain. True forward secrecy for channels would require interactive key exchange among all members, which is impractical over LoRa.
- Old keys are permanently deleted from both RAM and NVS after epoch advancement.

#### Public Channel ("Bramble Common")

Channel 0 is reserved as the default public channel. It uses a well-known PSK so all nodes can communicate without out-of-band key exchange:

```
Public Channel Key Derivation:
    well_known_psk = "bramble-default"
    channel_key = hkdf_sha256(
        salt = "bramble-channel-v1",
        ikm = sha256(well_known_psk),
        info = 0x00,                   // Channel ID 0
        length = 32
    )
```

**Properties:**
- **Name:** "Bramble Common"
- **Index:** 0 (reserved, cannot be deleted)
- **Hop limit:** 3 (configurable via `BRAMBLE_PUBLIC_CHANNEL_HOP_LIMIT`)
- **Purpose:** Town square for new node introduction, emergency broadcasts, general community messaging

**Rate limiting (stricter than private channels to prevent spam):**
- TX rate: 1 message per 30 seconds (`BRAMBLE_PUBLIC_CHANNEL_RATE_LIMIT_MS`)
- Burst allowance: 3 messages (token bucket)
- Per-source RX filter: drop if single source exceeds 1 msg/10s

**Security note:** The well-known PSK means any node can read Channel 0 messages. Use private channels or DMs for sensitive communication. Channel 0's encryption provides code path consistency but not confidentiality.

#### Group DM Key Management

Removed unshipped. The group-DM key manager (FNV-1a/BLAKE2s-derived group keys with epoch rotation) was deleted without ever being wired into the firmware or carried on the wire; its key derivation would not have survived review as a real KDF. Group messaging, if it returns, will be designed on top of the pairwise session keys from the cryptographic design RFC.

#### Out-of-band share URIs (webapp)

The webapp encodes channel, node, and network-key material distributed out-of-band (QR code or copy-paste) as `bramble://` URIs, all implemented in `webapp/src/utils/`:

```
Channel share:  bramble://ch/v1?n={name}&k={psk_hex}
    n   URL-encoded channel name (required)
    k   hex-encoded PSK (omitted when the channel has none)

Node share:     bramble://node/v1?n={name}&a={addr_hex}&pk={pubkey_base64url}
    n   URL-encoded node name
    a   hex address, no 0x prefix
    pk  base64url-encoded public key

Network key share: bramble://net/v1?k={key_hex}
    k   64 lowercase hex chars, the raw 32-byte network key
```

The network-key share is **write-only**: it exists solely to carry a freshly-generated key to each node's `bramble.setNetworkKey` RPC (§4.25 item 9) out of band, and the key is never read back from a device onto a share string. Instead, an operator verifies that provisioning converged across the fleet by comparing the `bramble.getNetworkKeyStatus` fingerprint on each node, `SHA256(key)[0:4]` as 8 lowercase hex chars, against the fingerprint of the key they generated. A matching fingerprint confirms the same key landed on both ends without either end ever transmitting the key itself a second time; it is not a short-authentication-string handshake and does not by itself authenticate the *node* being compared against, only the key material. See `docs/SECURITY-MODEL.md` §3 and §5 for what network-key provisioning does and does not close.

### 5.4 Privacy-Preserving Source Encryption for Route Discovery

The `encrypted_source` field in ROUTE_REQUEST must be decryptable only by the destination. We use a compact ECIES-like scheme:

```
function encrypt_source_for_rreq(my_addr, dest_pubkey):
    // Generate a throwaway ephemeral key
    eph_priv = random_bytes(32)
    eph_pub = x25519_base_point_mult(eph_priv)
    
    // Derive a one-time pad from DH
    shared = x25519(eph_priv, dest_pubkey)
    otp = sha256(shared || "bramble-rreq-v1")[0..3]  // 4 bytes
    
    encrypted_addr = my_addr XOR otp
    
    // We need to transmit eph_pub so the destination can decrypt.
    // But that's 32 bytes! Too big for inline.
    // Solution: Include a hint — the query_id IS derived from the ephemeral.
    // query_id = sha256(eph_pub)[0..3]
    // Destination tries all recent RREQ eph_pubs... No, too expensive.
    
    // Better solution: We use a deterministic ephemeral derived from 
    // a per-destination secret, so the destination can reconstruct it.
    // seed = sha256(static_dh(my_privkey, dest_pubkey) || "rreq" || current_hour_timestamp)
    // eph_priv = seed[0..31]
    
    // This means: the destination, knowing the static DH shared secret,
    // can derive the same ephemeral for any node it shares a DH secret with,
    // and try decryption. With max ~200 nodes, that's ~200 SHA-256 ops = trivial.
    
    // But we don't have a static DH shared secret if we've never communicated!
    // Solution: Use the destination's public key directly as the DH partner,
    // and embed a 4-byte hint from the ephemeral public key.
    
    // Final scheme:
    //   encrypted_source[0..3] = my_addr XOR sha256(x25519(eph_priv, dest_pubkey))[0..3]
    //   query_id[0..3] = sha256(eph_pub || dest_pubkey)[0..3]  (for dedup only, not crypto)
    //   The eph_pub is NOT transmitted. Instead, on receipt, the destination
    //   cannot decrypt without it. 
    
    // REVISED APPROACH — use static key:
    // Since we know dest_pubkey, we compute:
    shared_static = x25519(my_private_key, dest_pubkey)
    time_bucket = network_time_seconds / 3600      // Hourly bucket
    rreq_salt = random_bytes(4)                    // Per-RREQ random salt
    otp = sha256(shared_static || to_bytes_be32(time_bucket) || rreq_salt || "rreq")[0..3]
    encrypted_addr = my_addr XOR otp
    
    return (encrypted_addr, rreq_salt)
    // The rreq_salt is transmitted in the RREQ packet (see §4.6).
    // This prevents temporal correlation: repeated discoveries for the same
    // destination produce different encrypted_source values, so observers
    // cannot tell that two RREQs came from the same source.
    // Destination iterates its known peers' static DH secrets (using the
    // received salt) to find which one decrypts to a valid address.
    // With ~200 peers, this is ~200 SHA-256 operations — <10ms on ESP32.
```

```
function decrypt_rreq_source(encrypted_source, rreq_salt, current_time):
    time_bucket = current_time / 3600
    
    // Try current and previous hour (handles boundary cases)
    for tb in [time_bucket, time_bucket - 1]:
        for peer in known_peers:
            shared = cached_static_dh[peer]  // Or compute x25519(my_priv, peer.pubkey)
            otp = sha256(shared || to_bytes_be32(tb) || rreq_salt || "rreq")[0..3]
            candidate = encrypted_source XOR otp
            if candidate == peer.addr:
                return peer.addr
    
    // Unknown node — we can't decrypt. This is fine; we still relay the RREQ.
    // We'll learn their identity if they complete key exchange.
    return UNKNOWN_SOURCE
```

#### Dual-Mode RREQ Source: Encrypted vs. Open

The scheme above requires the RREQ originator to know the destination's public key (for static DH). This creates a bootstrapping problem for first-contact: how does node A discover a route to node B if A has never heard B's beacon and doesn't have B's public key?

**Option A — Encrypted source (default):** Pre-shared contacts mode. The source address is encrypted as described above. This requires the originator to have the destination's public key cached from a prior beacon or out-of-band exchange. This is the recommended mode for established meshes.

**Option B — Open source (fallback):** When `allow_open_rreq = true` (config flag, default: false), and the destination's public key is unknown, the RREQ is sent with the `OPEN_SOURCE` flag bit set (see §4.6) and `encrypted_source` contains the plaintext source address. This enables first-contact discovery at the cost of revealing the source to all relay nodes.

```
function encrypt_source_for_rreq(my_addr, dest_pubkey):
    if dest_pubkey == NULL:
        if config.allow_open_rreq:
            // First-contact mode: plaintext source
            return (my_addr, OPEN_SOURCE=1)
        else:
            // Contacts-only mode: cannot route to unknown destination
            return ERROR_NO_PUBKEY
    
    // Normal encrypted mode (as above)
    shared_static = x25519(my_private_key, dest_pubkey)
    time_bucket = network_time_seconds / 3600
    otp = sha256(shared_static || to_bytes_be32(time_bucket) || "rreq")[0..3]
    encrypted_addr = my_addr XOR otp
    return (encrypted_addr, OPEN_SOURCE=0)
```

**First-contact bootstrap flow:**
1. Node A sends open RREQ (plaintext source) to node B
2. Node B receives RREQ, sees A's address but has no shared secret → sends RREP with `auth_hmac = 0x00000000`
3. Node A receives RREP, installs route marked **"unverified"** (no HMAC authentication)
4. Node A initiates KEY_EXCHANGE over the unverified route
5. KEY_EXCHANGE completes → both nodes cache each other's public keys and derive session keys
6. Route promoted from "unverified" to **"active"**
7. Future RREQs between A and B use encrypted source (normal mode)

This explicitly resolves the KEY_EXCHANGE bootstrapping gap: KEY_EXCHANGE requires a route, and encrypted RREQ requires a public key (which comes from KEY_EXCHANGE). Open RREQ breaks the circular dependency for first contact.

### 5.5 Key Rotation

DM session keys rotate automatically:
- **Time-based:** Every 24 hours, a new key exchange is initiated automatically on next DM send.
- **Message-count-based:** After 2¹⁶ = 65,536 messages with the same key, force rekey (nonce space management).
- **Manual:** User can force rekey via the UI.

Channel keys rotate automatically via the epoch-based ratchet (see §5.3): every 24 hours or every 256 messages, whichever comes first. The base PSK must still be distributed out-of-band, but epoch rotation provides backward secrecy without requiring manual PSK redistribution.

### 5.6 NVS Key Storage Summary

```
Item                     Size      Count    Total
─────────────────────    ─────     ─────    ─────
Long-term private key    32 B      1        32 B
Long-term public key     32 B      1        32 B
Node address             4 B       1        4 B
Channel keys             32 B      16       512 B
Channel names            32 B      16       512 B
Channel epoch counters   2 B       16       32 B
DM session keys          32 B      32       1,024 B
DM key metadata          16 B      32       512 B
Peer public keys         32 B      64       2,048 B
────────────────────────────────────────────────
Total NVS:                                  4,708 B (~5KB)
```

---

## 6. Routing Protocol

### 6.1 Overview

Bramble uses a reactive (on-demand) routing protocol inspired by AODV, with significant privacy enhancements. Routes are discovered only when needed and cached until broken.

**Key differences from standard AODV:**
- Source address in RREQ is encrypted (only destination can read it)
- No sequence numbers broadcast globally (reduces metadata leakage)
- Path quality metrics weighted by link reliability, not just hop count
- Route cache entries have soft and hard timeouts

### 6.2 Data Structures

#### Routing Table

```c
struct route_entry {
    uint32_t dest_addr;       // 4 bytes — destination
    uint32_t next_hop;        // 4 bytes — next hop toward destination
    uint8_t  hop_count;       // 1 byte  — hops to destination
    uint8_t  metric;          // 1 byte  — route quality (255=best, 0=worst)
    uint8_t  flags;           // 1 byte  — ACTIVE, REPAIRING, STALE
    uint8_t  fail_count;      // 1 byte  — consecutive forwarding failures
    uint32_t last_used;       // 4 bytes — timestamp of last use (epoch seconds)
    uint32_t last_confirmed;  // 4 bytes — timestamp of last successful delivery
    uint16_t use_count;       // 2 bytes — times this route has been used
    uint16_t _padding;        // 2 bytes — alignment
};
// Size: 24 bytes per entry
// Max entries: 64
// Total: 1,536 bytes
```

#### Neighbor Table

```c
struct neighbor_entry {
    uint32_t addr;            // 4 bytes — neighbor address
    int8_t   rssi;            // 1 byte  — last RSSI
    int8_t   snr;             // 1 byte  — last SNR
    uint8_t  success_rate;    // 1 byte  — % of recent transmissions ACK'd (0–100)
    uint8_t  congestion;      // 1 byte  — last reported congestion level (0–3)
    uint32_t last_heard;      // 4 bytes — timestamp of last reception from this neighbor
    uint32_t pubkey_hash;     // 4 bytes — for key lookup
    uint16_t tx_count;        // 2 bytes — total transmissions to this neighbor
    uint16_t tx_success;      // 2 bytes — successful transmissions
};
// Size: 20 bytes per entry
// Max entries: 32
// Total: 640 bytes
```

#### Pending Route Discovery Table

```c
struct pending_discovery {
    uint32_t dest_addr;       // 4 bytes — who we're looking for
    uint32_t query_id;        // 4 bytes — RREQ query_id
    uint32_t timestamp;       // 4 bytes — when discovery started
    uint8_t  attempts;        // 1 byte  — RREQ attempts so far (max 3)
    uint8_t  _padding[3];     // 3 bytes
    // Queued packets waiting for this route
    uint8_t  queued_count;    // 1 byte  — packets queued for this dest
    uint8_t  _padding2[3];    // 3 bytes
};
// Size: 20 bytes per entry (excluding queued packet refs)
// Max entries: 8 concurrent discoveries
// Total: 160 bytes
```

#### RREQ Dedup Cache

```c
struct rreq_seen {
    uint32_t query_id;        // 4 bytes
    uint32_t timestamp;       // 4 bytes
};
// Size: 8 bytes per entry
// Max entries: 128
// Total: 1,024 bytes
// Eviction: entries older than 30 seconds are purged
```

#### Reverse Route Table (for RREP forwarding)

```c
struct reverse_route {
    uint32_t query_id;        // 4 bytes — from RREQ
    uint32_t prev_hop;        // 4 bytes — node that forwarded us this RREQ
    uint32_t timestamp;       // 4 bytes — when received
};
// Size: 12 bytes per entry
// Max entries: 32
// Total: 384 bytes
// Eviction: entries older than 60 seconds are purged
```

### 6.3 Route Discovery

**When a node wants to send to a destination with no cached route:**

```
function send_data(dest_addr, payload, tier):
    route = routing_table.lookup(dest_addr)
    
    if route != NULL and route.flags == ACTIVE:
        forward_data(route, payload, tier)
        return
    
    // No active route — initiate discovery
    if pending_discoveries.has(dest_addr):
        // Discovery already in progress — queue the packet
        pending_discoveries.queue_packet(dest_addr, payload, tier)
        return
    
    // Start new discovery
    query_id = random_uint32()
    enc_source = encrypt_source_for_rreq(my_addr, get_pubkey(dest_addr))
    
    rreq = build_rreq(
        dest_addr = dest_addr,
        query_id = query_id,
        encrypted_source = enc_source,
        hop_count = 0,
        metric = 255,
        prev_hop = my_addr
    )
    
    pending_discoveries.add(dest_addr, query_id, now())
    pending_discoveries.queue_packet(dest_addr, payload, tier)
    
    broadcast(rreq)
    
    // Schedule retry
    schedule_timer(RREQ_RETRY_1, 5000)  // 5 seconds
```

**Retry schedule for route discovery (expanding ring):**
- Attempt 1: immediate broadcast, hop_limit 4
- Attempt 2: after 5 seconds, hop_limit 8
- Attempt 3: 15 seconds after attempt 2, hop_limit 8
- After attempt 3: route discovery fails. Queued packets are dropped or returned to application.

Every attempt carries a **fresh `query_id`** (and therefore a fresh originator
pseudonym). Retries with the original query_id would be silently dropped by
every node whose 30-second RREQ dedup window still remembers the first flood;
a fresh query_id makes each retry a genuinely new flood. The originator's
pending-discovery entry remembers the query_id of every attempt, so an RREP
answering any outstanding attempt (including a late answer to an earlier one)
completes the discovery. When multiple attempts are answered, `route_install`
keeps the route with the better metric.

**KEY_EXCHANGE reliability:** KEY_EXCHANGE packets are sent with Critical-tier reliability (8 retries with exponential backoff). This ensures key establishment completes even under adverse radio conditions, since a failed key exchange blocks all subsequent DM communication.

**Passive route learning:** Nodes also learn routes passively from beacon route advertisements (see §4.9). When a beacon contains route ads, the receiver can install or refresh routes without initiating RREQ flooding. This significantly reduces routing overhead in stable meshes.

**RREQ Forwarding (intermediate nodes):**

```
function handle_rreq(rreq, rx_rssi, rx_snr):
    // Dedup check
    if rreq_dedup.has(rreq.query_id):
        return  // Already processed this RREQ
    
    rreq_dedup.add(rreq.query_id, now())
    
    // Am I the destination?
    if rreq.header.dest_addr == my_addr:
        handle_rreq_at_destination(rreq)
        return
    
    // Check hop limit
    if rreq.header.hop_limit <= 1:
        return  // TTL expired
    
    // Store reverse route (for forwarding RREP back)
    reverse_routes.add(rreq.query_id, rreq.prev_hop, now())
    
    // Compute link quality penalty
    link_penalty = compute_link_penalty(rx_rssi, rx_snr)
    
    // Forward with updates
    rreq.hop_count += 1
    rreq.metric = max(0, rreq.metric - link_penalty)
    rreq.header.hop_limit -= 1
    rreq.prev_hop = my_addr
    
    // Jittered rebroadcast to reduce collisions: without it, every
    // same-hop relay keys up at the same instant and the flood collides
    // with itself
    delay = random(50, 300)  // ms
    schedule_broadcast(rreq, delay)
```

```
function compute_link_penalty(rssi, snr):
    // Score 0 (excellent) to 50 (marginal)
    // RSSI: -60 dBm = excellent, -120 dBm = barely usable
    rssi_penalty = clamp(((-60) - rssi) / 2, 0, 30)  // 0–30
    // SNR: 10 dB = great, -5 dB = poor  
    snr_penalty = clamp((10 - snr) * 2, 0, 20)       // 0–20
    return min(rssi_penalty + snr_penalty, 50)
```

**RREQ handling at destination:**

```
function handle_rreq_at_destination(rreq):
    // Decrypt source
    source_addr = decrypt_rreq_source(rreq.encrypted_source, now())
    
    // Store reverse route for this query
    reverse_routes.add(rreq.query_id, rreq.prev_hop, now())
    
    // Build and install route to source (via prev_hop)
    routing_table.install(
        dest = source_addr,     // May be UNKNOWN_SOURCE if first contact
        next_hop = rreq.prev_hop,
        hop_count = rreq.hop_count + 1,
        metric = rreq.metric,
        flags = ACTIVE
    )
    
    // Compute RREP authentication HMAC
    if source_addr != UNKNOWN_SOURCE:
        shared = cached_static_dh[source_addr]  // Static DH with RREQ originator
        hmac_data = rrep_header_bytes[0..25]
        auth_hmac = hmac_sha256(shared, hmac_data)[0..7]  // Truncated to 8 bytes
    else:
        auth_hmac = 0x00000000  // First contact — no shared secret
    
    // Send ROUTE_REPLY back along reverse path
    rrep = build_rrep(
        dest_addr = rreq.prev_hop,    // Next hop back
        query_id = rreq.query_id,
        src_addr = my_addr,
        next_hop = rreq.prev_hop,
        hop_count = rreq.hop_count + 1,
        route_metric = rreq.metric,
        auth_hmac = auth_hmac
    )
    
    send_unicast(rreq.prev_hop, rrep)
```

**RREP forwarding (intermediate nodes):**

```
function handle_rrep(rrep, rx_rssi):
    // Install forward route to the RREP originator (the RREQ destination)
    routing_table.install(
        dest = rrep.src_addr,
        next_hop = prev_transmitter,   // Node that sent us this RREP
        hop_count = rrep.hop_count,
        metric = rrep.route_metric,
        flags = ACTIVE
    )
    
    // Look up reverse route to forward RREP back toward RREQ originator
    rev = reverse_routes.lookup(rrep.query_id)
    if rev == NULL:
        return  // Stale or unknown query
    
    rrep.next_hop = rev.prev_hop
    send_unicast(rev.prev_hop, rrep)
```

**RREP arrival at RREQ originator:**

```
function handle_rrep_at_origin(rrep):
    discovery = pending_discoveries.lookup_by_query(rrep.query_id)
    if discovery == NULL:
        return  // Unknown or expired discovery
    
    // Verify RREP authentication
    route_flags = ACTIVE
    if rrep.auth_hmac == 0x00000000:
        // First-contact: no HMAC — route is unverified until KEY_EXCHANGE
        route_flags = UNVERIFIED
    else:
        shared = cached_static_dh[discovery.dest_addr]
        expected = hmac_sha256(shared, rrep_bytes[0..25])[0..7]
        if expected != rrep.auth_hmac:
            log("RREP HMAC verification failed for %s", discovery.dest_addr)
            return  // Drop unauthenticated RREP
    
    // Install route
    routing_table.install(
        dest = discovery.dest_addr,
        next_hop = prev_transmitter,
        hop_count = rrep.hop_count,
        metric = rrep.route_metric,
        flags = route_flags
    )
    
    // Send all queued packets for this destination
    for pkt in discovery.queued_packets:
        forward_data(routing_table.lookup(discovery.dest_addr), pkt.payload, pkt.tier)
    
    pending_discoveries.remove(discovery.dest_addr)
```

### 6.4 Route Maintenance

#### Route Timeouts

```
ROUTE_ACTIVE_TIMEOUT   = 300 seconds  (5 min — route goes STALE if unused)
ROUTE_STALE_TIMEOUT    = 600 seconds  (10 min — stale route is deleted)
ROUTE_HARD_TIMEOUT     = 3600 seconds (1 hr — route deleted even if recently used)
```

```
function route_maintenance_tick():  // Called every 10 seconds
    now = current_time()
    
    for route in routing_table:
        age = now - route.last_used
        confirmed_age = now - route.last_confirmed
        
        if age > ROUTE_HARD_TIMEOUT:
            routing_table.remove(route)
        elif route.flags == STALE and age > ROUTE_STALE_TIMEOUT:
            routing_table.remove(route)
        elif route.flags == ACTIVE and age > ROUTE_ACTIVE_TIMEOUT:
            route.flags = STALE
        
        // Also mark as stale if not confirmed for a long time
        if route.flags == ACTIVE and confirmed_age > ROUTE_ACTIVE_TIMEOUT * 2:
            route.flags = STALE
```

#### Broken Link Detection

When a forwarding node fails to receive a link-layer ACK (or Bramble-layer ACK for tier ≥ Normal) from the next hop after retries:

```
function handle_forwarding_failure(dest_addr, next_hop, failed_packet):
    route = routing_table.lookup(dest_addr)
    if route == NULL:
        return
    
    route.fail_count += 1
    
    if route.fail_count >= 3:
        // Route is broken
        route.flags = BROKEN
        
        // Send ROUTE_ERROR back toward the source
        rerr = build_route_error(
            reporter = my_addr,
            broken_dest = dest_addr,
            broken_next_hop = next_hop
        )
        
        // Propagate RERR to all nodes that might be using this route
        // (i.e., the node that sent us the failed packet)
        send_unicast(failed_packet.prev_transmitter, rerr)
        
        // Attempt local repair if we know an alternate path
        alt_route = routing_table.find_alternate(dest_addr, exclude=next_hop)
        if alt_route != NULL:
            routing_table.install(dest_addr, alt_route.next_hop, 
                alt_route.hop_count, alt_route.metric, ACTIVE)
            // Retry the failed packet on the new route
            forward_data(alt_route, failed_packet.payload, failed_packet.tier)
        else:
            // Queue packet and initiate new route discovery
            initiate_route_discovery(dest_addr, failed_packet)
```

#### RERR Propagation

```
function handle_route_error(rerr):
    route = routing_table.lookup(rerr.broken_dest)
    
    if route != NULL and route.next_hop == rerr.broken_next_hop:
        route.flags = BROKEN
        routing_table.remove(route)
        
        // Propagate to any upstream node that might be using this route
        // We know about upstream nodes from delivery receipts' relay paths
        for upstream in nodes_using_route(rerr.broken_dest):
            send_unicast(upstream, rerr)
```

### 6.5 Routing State Machine

Each route entry transitions through these states:

```
                     ┌─────────────┐
          RREQ sent  │   DISCOVERING│
       ┌────────────►│  (no route)  │
       │             └──────┬───────┘
       │                    │ RREP received
       │                    ▼
       │             ┌─────────────┐
       │    (HMAC=0) │  UNVERIFIED  │ First-contact: RREP had no HMAC
       │             │  (untrusted) │
       │             └──────┬───────┘
       │                    │ KEY_EXCHANGE completes successfully
       │                    ▼
       │             ┌─────────────┐
       │             │    ACTIVE    │◄──── Route confirmed by ACK/receipt (or authenticated RREP)
       │             │  (in use)    │────┐
       │             └──────┬───────┘    │ Successful delivery resets timer
       │                    │            │
       │                    │ Unused for 300s
       │                    ▼
       │             ┌─────────────┐
       │             │    STALE     │
       │             │  (aged out)  │
       │             └──────┬───────┘
       │                    │
       │      ┌─────────────┤
       │      │ Used again  │ Unused for 600s more
       │      │ (promote)   │
       │      ▼             ▼
       │   ACTIVE       ┌─────────────┐
       │                │   DELETED    │
       │                │  (removed)   │
       │                └─────────────┘
       │
       │  3 consecutive          ┌─────────────┐
       │  forwarding failures    │   BROKEN     │
       │  ─────────────────────► │  (link down) │
       │                         └──────┬───────┘
       │                                │
       │       Local repair found       │ No alternate
       │       ──────────────────       │ ───────────
       │       Transition to ACTIVE     │
       │                                ▼
       └──────────────────────── Re-enter DISCOVERING
```

### 6.6 Data Forwarding

```
function forward_data(route, packet):
    if route.flags != ACTIVE:
        if route.flags == STALE:
            // Promote back to active and use
            route.flags = ACTIVE
            route.fail_count = 0
        else:
            initiate_route_discovery(packet.dest_addr, packet)
            return
    
    packet.next_hop = route.next_hop
    packet.header.hop_limit -= 1
    
    if packet.header.hop_limit <= 0:
        drop(packet, "TTL expired")
        return
    
    route.last_used = now()
    route.use_count += 1
    
    enqueue_tx(packet, priority_from_tier(packet.tier))
```

### 6.7 Channel (Group) Message Routing

> **Firmware reality.** The hop-limited flood relay below is not implemented: received broadcast/channel DATA is consumed locally and never rebroadcast, so channel messages reach direct radio neighbors only. The `channel_flood` module that implemented this decision logic was deleted unshipped after sitting caller-less; its 50-300 ms jitter window lives on in the RREQ forward path.

Channel messages use a limited controlled flood scoped by hop_limit:

```
function send_channel_message(channel_id, payload):
    // Channel messages are broadcast with a limited hop_limit
    pkt = build_channel_data(
        channel_id = channel_id,
        payload = payload,
        hop_limit = min(channel_config[channel_id].hop_limit, 4)  // Default 3 hops
    )
    
    broadcast(pkt)
```

```
function handle_channel_data_relay(pkt):
    // Channel ID is inside the ciphertext — relay nodes cannot determine which channel
    // this belongs to. Relay unconditionally if CHANNEL flag is set.
    
    if pkt.header.hop_limit <= 1:
        return  // Don't relay further
    
    if packet_dedup.has(pkt.header.packet_id):
        return  // Already relayed
    
    packet_dedup.add(pkt.header.packet_id, now())
    
    // Decrement hop limit and rebroadcast
    pkt.header.hop_limit -= 1
    
    // Jittered delay to reduce collision with other relayers
    delay = random(50, 300)  // ms
    schedule_broadcast(pkt, delay)
```

Channel messages flood but are scoped: default max 3 hops (configurable per channel, max 6). This is an acceptable tradeoff since channels are "public" within their membership and typically used for less time-sensitive group communication.

### 6.8 Route Metric and Selection

Path quality is tracked with a single **penalty-accumulating metric**
(0-255, higher = better). An RREQ starts at 255; every relay subtracts a
link penalty derived from the RSSI and SNR it received the RREQ with
(`compute_link_penalty`: 0-30 points for RSSI between -60 and -120 dBm,
0-20 points for SNR between +10 and -5 dB). The destination echoes the
accumulated metric back in the RREP, and the originator and each reverse
relay subtract their own receive-link penalty when installing the route.

**Selection is first-arrival within a single flood.** Relays forward only
the first copy of a query they hear (RREQ dedup, 30-second window), so one
flood explores one path per relay and the first copy to reach the
destination defines the candidate route. Duplicate copies arriving over
other paths are dropped regardless of metric.

**The metric arbitrates between floods, not within one.** `route_install`
replaces an existing route only when the candidate has a strictly better
metric (or equal metric with fewer hops), or when the existing route is
broken or stale. Because every discovery attempt floods under a fresh
query_id, a discovery that needed a retry can produce multiple RREPs, and
the best one wins.

A weighted composite metric (delivery-rate and latency EMAs, airtime
score, switch hysteresis) was specified here previously and existed in the
tree as `route_metric.c`, but it was never wired into route selection:
first-arrival dedup decided routes alone. Simulation of the wired-up
version against the collision model showed no measurable delivery or
latency change on the 10- and 50-node scenarios
(`docs/results/simulation-2026-06.md`, DES-4), so the module was deleted
rather than shipped as decoration.

---

## 7. Reliability Layer

### 7.1 Three-Tier Model

| Tier | Flag Bits | ACK Required | Max Retries | Retry Backoff | Use Case |
|------|-----------|-------------|-------------|---------------|----------|
| **Critical** (10) | `TIER=10, ACK_REQ=1, RECEIPT=1` | End-to-end ACK + delivery receipt with relay path | 8 | Exponential: 3s, 6s, 12s, 24s, 48s, 96s, 192s, 384s | Emergency alerts, key exchange |
| **Normal** (01) | `TIER=01, ACK_REQ=1, RECEIPT=0` | End-to-end ACK only (no relay path) | 3 | Exponential: 2s, 4s, 8s | Text messages, general communication |
| **Broadcast** (00) | `TIER=00, ACK_REQ=0` | None | 0 | N/A | Beacons, telemetry, sensor data |

### 7.2 ACK Mechanics

**End-to-end ACKs** travel from the destination back to the source along the reverse route:

```
function handle_data_at_destination(pkt):
    // Process the data...
    deliver_to_application(pkt)
    
    if pkt.flags.ACK_REQ:
        ack = build_ack(
            dest_addr = get_source(pkt),
            ack_packet_id = pkt.header.packet_id,
            ack_flags = SUCCESS,
            rssi = rx_rssi + 128  // Offset encoding
        )
        
        route = routing_table.lookup(get_source(pkt))
        if route != NULL:
            send_unicast(route.next_hop, ack)
        else:
            // No reverse route known — attempt to send via prev_transmitter
            send_unicast(pkt.prev_transmitter, ack)
    
    if pkt.flags.RECEIPT:
        receipt = build_delivery_receipt(
            dest_addr = get_source(pkt),
            orig_packet_id = pkt.header.packet_id,
            hop_count = 0,           // Will be incremented by relays
            total_latency = 0,       // Will be computed by relays
            relay_path = [my_addr]   // Start with self
        )
        
        route = routing_table.lookup(get_source(pkt))
        if route != NULL:
            send_unicast(route.next_hop, receipt)
```

**Delivery receipt relay path building:**

```
function handle_delivery_receipt_relay(receipt):
    // Append my address to the relay path
    if receipt.hop_count < 8:
        receipt.relay_path[receipt.hop_count] = my_addr
        receipt.hop_count += 1
    
    // Add my contribution to latency estimate
    receipt.total_latency += estimate_my_queue_delay()
    
    // Forward toward the original sender
    route = routing_table.lookup(receipt.header.dest_addr)
    if route != NULL:
        send_unicast(route.next_hop, receipt)
```

### 7.3 Retry Mechanics with Exponential Backoff

```
struct pending_ack {
    uint32_t packet_id;
    uint32_t dest_addr;
    uint8_t  tier;
    uint8_t  attempt;         // 0-indexed retry count
    uint8_t  max_attempts;    // 3 for Normal, 8 for Critical
    uint8_t  _padding;
    uint32_t next_retry_ms;   // Timestamp for next retry
    uint16_t packet_len;
    uint8_t  packet_data[222]; // Cached packet for retransmission
};
// Size: ~236 bytes per entry
// Max entries: 8 (concurrent unacknowledged packets)
// Total: 1,888 bytes
```

```
function schedule_retry(pending):
    base_delay_ms = (pending.tier == CRITICAL) ? 3000 : 2000
    backoff = base_delay_ms * (1 << pending.attempt)  // Exponential
    jitter = random(0, backoff / 4)                    // 25% jitter
    pending.next_retry_ms = now_ms() + backoff + jitter

function retry_tick():  // Called every 500ms
    now = now_ms()
    
    for entry in pending_acks:
        if entry.next_retry_ms > now:
            continue
        
        if entry.attempt >= entry.max_attempts:
            // Give up
            notify_application(DELIVERY_FAILED, entry.packet_id, entry.dest_addr)
            
            // If this was a routing failure, trigger route error
            if entry.attempt >= 2:  // Failed at least twice via this route
                handle_forwarding_failure(entry.dest_addr, 
                    routing_table.lookup(entry.dest_addr).next_hop, entry)
            
            pending_acks.remove(entry)
            continue
        
        entry.attempt += 1
        
        // For Critical tier, try alternate route after 3 failures on primary
        if entry.tier == CRITICAL and entry.attempt == 3:
            alt = routing_table.find_alternate(entry.dest_addr)
            if alt != NULL:
                update_packet_next_hop(entry.packet_data, alt.next_hop)
        
        // Re-enqueue for transmission
        enqueue_tx(entry.packet_data, entry.packet_len, priority_from_tier(entry.tier))
        schedule_retry(entry)
```

### 7.4 Retry Timing Summary

**Normal tier:**
| Attempt | Delay | Cumulative |
|---------|-------|------------|
| 1 (initial) | 0s | 0s |
| 2 | 2–2.5s | 2–2.5s |
| 3 | 4–5s | 6–7.5s |
| 4 | 8–10s | 14–17.5s |
| Give up | — | ~15s total |

**Critical tier:**
| Attempt | Delay | Cumulative |
|---------|-------|------------|
| 1 (initial) | 0s | 0s |
| 2 | 3–3.75s | 3–3.75s |
| 3 | 6–7.5s | 9–11.25s |
| 4 | 12–15s | 21–26.25s |
| 5 | 24–30s | 45–56.25s |
| 6 | 48–60s | 93–116.25s |
| 7 | 96–120s | 189–236.25s |
| 8 | 192–240s | 381–476.25s |
| 9 | 384–480s | 765–956.25s |
| Give up | — | ~12–16 min total |

### 7.5 Sliding Window Flow Control

Removed unshipped. The per-destination AIMD sliding window was deleted without ever being wired into the transmit path; sender pacing comes from the per-tier airtime budget (section 8) and the bounded pending-ACK table (8 entries), which caps in-flight unacknowledged traffic.

### 7.6 Duplicate Detection

Every received packet is checked against a dedup buffer to prevent processing duplicates:

```
struct dedup_entry {
    uint32_t packet_id;      // 4 bytes
    uint32_t timestamp;      // 4 bytes
};
// 8 bytes × 256 entries = 2,048 bytes

function is_duplicate(packet_id):
    // Purge entries older than 60 seconds
    dedup_buffer.purge_older_than(now() - 60)
    
    if dedup_buffer.has(packet_id):
        return true
    
    dedup_buffer.add(packet_id, now())
    return false
```

---

## 8. Airtime Budget System

### 8.1 Regulatory Context

US ISM 902–928 MHz: No strict duty cycle requirement (unlike EU 868 MHz), but FCC Part 15.247 requires either frequency hopping or digital modulation with max 1W conducted power. Bramble uses single-channel operation at +22 dBm, which is compliant for digitally-modulated systems with ≥500 kHz bandwidth at SF ≤ 8, or with the frequency hopping variant.

Regardless of legality, Bramble enforces a **self-imposed 10% airtime budget** per node to ensure mesh scalability. At SF10/125kHz, 10% duty cycle = ~360 seconds of airtime per hour, which is ~420 packets of 200 bytes or ~750 packets of 100 bytes per hour.

### 8.2 Token Bucket Algorithm

#### 8.2.1 Implementation Snapshot (current firmware)

Current firmware uses a per-tier token bucket with **continuous refill** (not hourly cliff resets). Tokens are refilled proportionally to elapsed time and capped at per-tier maxima.

Every transmission is admitted through a single budget-gated TX path (`components/radio/tx_gate.c`): packets are classified by kind, costed with real time-on-air math, budget-checked, passed through listen-before-talk, then transmitted and debited. The raw radio transmit call is internal to the radio component, so no code path bypasses the budget. When the regional frequency plan enforces a regulatory duty-cycle limit (EU868: 1%), the tier maxima and refill are scaled to stay within it (`airtime_budget_set_duty_cap`); the US plan carries no enforced cap, and the self-imposed budgets below apply.

Tier budgets (base):
- `critical`: 36000 ms/hour
- `normal`: 18000 ms/hour
- `broadcast`: 18000 ms/hour
- `receipt`: 12000 ms/hour

Receipt traffic (broadcast delivery receipts + forwarded receipts) uses its own `receipt` tier so receipt storms do not directly consume broadcast-data tokens.

Adaptive mesh-size profile (by peer count):
- `<=15` peers (small mesh): relaxed budgets (normal +50%, broadcast/receipt +100%)
- `16..40` peers: baseline budgets
- `>40` peers (large mesh): conservative budgets (normal 75%, broadcast 60%, receipt 50%)

This profile is applied at runtime via `airtime_budget_set_mesh_size(...)` and surfaced in airtime RPC output (`receipt_remaining_ms`, `receipt_max_ms`, etc.).

```
struct airtime_budget {
    uint32_t tokens_us;          // Current tokens in microseconds
    uint32_t max_tokens_us;      // Bucket capacity
    uint32_t refill_rate_us;     // Tokens added per second (µs/s)
    uint32_t last_refill_time;   // Last refill timestamp (ms)
    
    // Per-tier sub-budgets (percentage of total)
    uint32_t critical_tokens_us;   // 40% of budget
    uint32_t normal_tokens_us;     // 40% of budget
    uint32_t broadcast_tokens_us;  // 20% of budget
};

// Budget parameters (LongRange profile, 10% duty cycle):
// Total: 100,000 µs per second (10% of 1,000,000 µs)
// Bucket capacity: 10,000,000 µs (100 seconds of accumulated airtime)
//   - Allows bursts but prevents sustained overuse
// Critical sub-bucket: 4,000,000 µs max, 40,000 µs/s refill
// Normal sub-bucket:   4,000,000 µs max, 40,000 µs/s refill
// Broadcast sub-bucket: 2,000,000 µs max, 20,000 µs/s refill
```

```
function refill_tokens():
    now = now_ms()
    elapsed_ms = now - budget.last_refill_time
    budget.last_refill_time = now
    
    elapsed_s = elapsed_ms / 1000.0
    
    budget.critical_tokens_us = min(
        budget.critical_tokens_us + (uint32_t)(40000 * elapsed_s),
        4000000
    )
    budget.normal_tokens_us = min(
        budget.normal_tokens_us + (uint32_t)(40000 * elapsed_s),
        4000000
    )
    budget.broadcast_tokens_us = min(
        budget.broadcast_tokens_us + (uint32_t)(20000 * elapsed_s),
        2000000
    )

function can_transmit(packet_airtime_us, tier):
    refill_tokens()
    
    match tier:
        CRITICAL:
            // Critical can borrow from normal and broadcast pools
            available = budget.critical_tokens_us + budget.normal_tokens_us + budget.broadcast_tokens_us
            return available >= packet_airtime_us
        NORMAL:
            // Normal can borrow from broadcast pool only
            available = budget.normal_tokens_us + budget.broadcast_tokens_us
            return available >= packet_airtime_us
        BROADCAST:
            return budget.broadcast_tokens_us >= packet_airtime_us

function debit_airtime(packet_airtime_us, tier):
    remaining = packet_airtime_us
    
    match tier:
        CRITICAL:
            deducted = min(remaining, budget.critical_tokens_us)
            budget.critical_tokens_us -= deducted
            remaining -= deducted
            if remaining > 0:
                deducted = min(remaining, budget.normal_tokens_us)
                budget.normal_tokens_us -= deducted
                remaining -= deducted
            if remaining > 0:
                budget.broadcast_tokens_us -= remaining
        
        NORMAL:
            deducted = min(remaining, budget.normal_tokens_us)
            budget.normal_tokens_us -= deducted
            remaining -= deducted
            if remaining > 0:
                budget.broadcast_tokens_us -= remaining
        
        BROADCAST:
            budget.broadcast_tokens_us -= packet_airtime_us
```

### 8.3 Airtime Calculation

```
function calculate_airtime_us(payload_bytes, sf, bw_hz, cr):
    // LoRa airtime calculation per Semtech AN1200.13
    
    n_preamble = 12  // Configured preamble symbols
    
    // Symbol duration
    t_sym_us = (1 << sf) * 1000000 / bw_hz
    
    // Preamble time
    t_preamble_us = (n_preamble + 4.25) * t_sym_us
    
    // Payload symbols
    // Using explicit header, CRC enabled, low data rate optimize for SF >= 11
    de = 1 if (sf >= 11 and bw_hz == 125000) else 0
    ih = 0  // Explicit header
    
    numerator = 8 * payload_bytes - 4 * sf + 28 + 16 - 20 * ih
    denominator = 4 * (sf - 2 * de)
    
    n_payload_symbols = 8 + max(ceil(numerator / denominator) * (cr + 4), 0)
    
    t_payload_us = n_payload_symbols * t_sym_us
    
    return t_preamble_us + t_payload_us

// Examples (SF10, 125kHz, CR 4/6 = cr=2):
// 22 bytes (ACK):    ~290 ms = 290,000 µs
// 36 bytes (beacon): ~400 ms = 400,000 µs
// 100 bytes (short msg): ~480 ms = 480,000 µs
// 200 bytes (full msg):  ~850 ms = 850,000 µs
// 222 bytes (max):       ~920 ms = 920,000 µs
```

### 8.4 Congestion Detection and Response

Removed unshipped. The queue-depth congestion assessment and CONGESTION broadcast designed here were deleted without ever being wired; the firmware's only admission control is the budget-gated TX chokepoint (section 8.2.1), which bounds every tier including retries.

### 8.5 TX Queue and Priority Scheduling

Removed unshipped. The priority TX queue was deleted without ever being wired: the live transmit path is the budget-gated TX chokepoint (section 8.2.1), which performs admission and LBT inline and never queues. The beacon's `tx_queue_depth` wire field remains in the format and is always reported as 0; retiring the field is deferred to the next wire-version bump.

---

## 9. Time Synchronization

### 9.1 Design Goals

- Loose synchronization: ±1–2 seconds across the mesh
- No GPS required (but GPS-equipped nodes serve as stratum-0 sources)
- Sufficient for anti-replay windows (±30 seconds) and route expiry
- Minimal airtime overhead

### 9.2 Beacon-Based Sync Protocol

Every node includes its current time estimate and confidence in its BEACON packet (§4.9). Additionally, dedicated TIME_SYNC packets provide higher-precision synchronization.

> **Firmware reality.** Only the beacon-carried sync is implemented (`timesync_handle_sync` on beacon receipt in `main/mesh_task.c`); the dedicated `TIME_SYNC` packet type was removed unshipped (section 4.13). No stratum-0 source is wired yet (no GPS or operator seed), so nodes exchange offsets but none can bootstrap the mesh to synchronized absolute time.

**Stratum model:**
- Stratum 0: GPS-equipped node with valid fix (confidence = 0 ms)
- Stratum 1: Direct neighbor of stratum-0 node (confidence = estimated OTA delay)
- Stratum N: N hops from nearest stratum-0 (confidence grows with each hop)
- Stratum 15: Maximum (unsynced / no reference)

**If no GPS node exists:** The node with the longest uptime and most neighbors becomes the de facto time reference (stratum 7). All other nodes sync relative to it. The specific stratum is a convention: the mesh converges regardless.

### 9.3 Sync Algorithm

```
struct time_state {
    uint32_t network_time;      // Current network time estimate (epoch seconds)
    uint32_t local_offset;      // network_time = local_millis/1000 + local_offset
    uint16_t confidence_ms;     // Uncertainty in our time estimate
    uint8_t  stratum;           // Our distance from best time source
    uint8_t  sync_source;       // Address of our current sync source (best peer)
    uint32_t last_sync;         // When we last synced
};
```

```
function handle_time_sync(pkt, rx_timestamp_local_ms):
    // Ignore if from a worse stratum (unless we have no sync at all)
    if pkt.stratum >= my_time.stratum and my_time.stratum < 15:
        return
    
    // Ignore if packet confidence is worse than ours
    if pkt.confidence_ms >= my_time.confidence_ms and my_time.stratum <= pkt.stratum + 1:
        return
    
    // Estimate one-way delay (we know packet airtime from packet size)
    packet_airtime_ms = calculate_airtime_us(pkt.length, SF, BW, CR) / 1000
    
    // The sender's time at transmission was pkt.timestamp
    // We received it at rx_timestamp_local_ms (our local clock)
    // Network time right now ≈ pkt.timestamp + packet_airtime_ms/1000
    
    estimated_network_time = pkt.timestamp  // Sender's timestamp at TX
    estimated_delay_ms = packet_airtime_ms + 10  // Airtime + ~10ms processing
    
    new_offset = estimated_network_time - (rx_timestamp_local_ms / 1000) + (estimated_delay_ms / 1000)
    
    // Clamp maximum time shift per sync interval — no single source can shift
    // our clock more than 5 seconds per sync
    MAX_SHIFT_S = 5
    current_offset = my_time.local_offset
    if abs(new_offset - current_offset) > MAX_SHIFT_S and my_time.stratum < 15:
        // Large shift — clamp to ±5 seconds
        if new_offset > current_offset:
            new_offset = current_offset + MAX_SHIFT_S
        else:
            new_offset = current_offset - MAX_SHIFT_S
    
    // Stratum-0 claims require corroboration: at least 2 neighbors must report
    // consistent stratum-0 derived time within 2 beacon cycles before adoption
    if pkt.stratum == 0 and not stratum0_corroborated(pkt.src_addr):
        // Record this claim; don't adopt yet
        stratum0_claims.record(pkt.src_addr, new_offset, now())
        // Check if we have corroboration from 2+ neighbors within 2 beacon cycles
        if stratum0_claims.corroborated_count(new_offset, tolerance=2s, window=2*BEACON_INTERVAL) >= 2:
            stratum0_claims.mark_corroborated(pkt.src_addr)
        else:
            return  // Wait for corroboration
    
    // Weighted moving average to avoid jumps
    // Weight: 0.3 for new value (slow adaptation to avoid oscillation)
    if my_time.stratum < 15:  // We have some existing sync
        alpha = 0.3
        my_time.local_offset = (uint32_t)(alpha * new_offset + (1.0 - alpha) * my_time.local_offset)
    else:
        // First sync — accept immediately
        my_time.local_offset = new_offset
    
    my_time.stratum = pkt.stratum + 1
    my_time.confidence_ms = pkt.confidence_ms + estimated_delay_ms
    my_time.sync_source = pkt.src_addr
    my_time.last_sync = rx_timestamp_local_ms

function get_network_time():
    return millis() / 1000 + my_time.local_offset
```

### 9.4 Beacon Sync Integration

On receiving any beacon:

```
function process_beacon_time(beacon, rx_time_local_ms):
    // Beacons carry time with lower precision than TIME_SYNC packets
    // but are more frequent, so use them as a background sync source
    
    if beacon.time_confidence < my_time.confidence_ms - 500:
        // This beacon has significantly better time than us
        // Run a simplified version of the sync algorithm
        handle_time_sync_from_beacon(beacon, rx_time_local_ms)
```

### 9.5 TIME_SYNC Packet Emission

Removed unshipped. Proactive TIME_SYNC emission was deleted along with the packet type (section 4.13); every node shares time passively via its beacons, which is the only sync transport.

### 9.6 Anti-Replay Timestamp Windows

> **Firmware reality.** Not implemented; the `anti_replay` module that implemented this design was deleted unshipped, and replay protection is being redesigned around dedup on authenticated fields. Today only the packet-id dedup buffer (section 7.6) ships.

```
REPLAY_WINDOW_S = 30   // ±30 seconds

function check_replay(pkt_timestamp, packet_id):
    network_now = get_network_time()
    
    // Check timestamp is within window
    if abs(pkt_timestamp - network_now) > REPLAY_WINDOW_S:
        drop(pkt, "timestamp outside replay window")
        return REPLAY_DETECTED
    
    // Check packet_id not seen recently
    if is_duplicate(packet_id):
        drop(pkt, "duplicate packet_id")
        return REPLAY_DETECTED
    
    return OK
```

Note: The ±30s window is deliberately large to accommodate:
- Time sync uncertainty (±1-2s in well-connected mesh, up to ±5s at edges)
- Multi-hop propagation delay (up to ~8 seconds for 8-hop path with queuing)
- Clock drift between sync events (~10 ppm = 3s per 5-minute sync interval)

### 9.7 Convergence Properties

- **With GPS node:** Network converges to ±100ms within 3 beacon cycles (~3 minutes)
- **Without GPS:** Network converges to ±2s within 5 beacon cycles (~5 minutes). Absolute time may drift but relative synchronization is maintained.
- **Partition and rejoin:** When a network partition heals, the higher-stratum partition adopts the lower-stratum partition's time within 2 sync intervals (~10 minutes).

---

## 10. Security Analysis

> **Firmware reality.** This section analyzes the *design*. [SECURITY-MODEL.md](SECURITY-MODEL.md) is the authoritative, code-verified statement of the current security posture, including the gaps where the implementation does not yet deliver the properties analyzed here (no pairwise DM keys, unauthenticated routing control, no replay protection, plaintext location packets, and more).

### 10.1 Threat Model

**Attacker capabilities (assumed):**
1. **Passive listener:** Can receive all LoRa transmissions within radio range. Has a Software Defined Radio (SDR) and can record all traffic.
2. **Active attacker:** Can transmit crafted LoRa packets. Owns Bramble-compatible hardware.
3. **Mesh participant:** Has joined the mesh with a valid node identity. May be malicious.
4. **Non-goals:** We do NOT defend against a global passive adversary who can monitor all radio traffic across the entire mesh simultaneously (state-level). We DO defend against a local adversary monitoring a subset of the mesh.

### 10.2 Protections

#### 10.2.1 Confidentiality

| Threat | Protection | Residual Risk |
|--------|-----------|---------------|
| DM content interception | AES-256-GCM with per-pair X25519-derived keys. Relay nodes see only ciphertext. | Compromised endpoint reveals DM content for that pair only. |
| Channel message interception | AES-256-GCM with channel PSK + epoch ratchet. Non-members cannot decrypt. Epoch rotation provides backward secrecy. | Any channel member can read all channel messages (by design). Current key compromise exposes future messages (attacker can derive forward epochs). Past messages protected by backward secrecy. |
| Key exchange interception | X25519 Double-DH + ephemeral keys provide forward secrecy. | If both parties' long-term keys are compromised, past sessions (before last rekey) are exposed. |

#### 10.2.2 Integrity & Authentication

| Threat | Protection |
|--------|-----------|
| Packet tampering | AES-256-GCM auth tag (16 bytes) on all encrypted packets. Any modification is detected. |
| Spoofed source address | DM: Source verified implicitly — only the real source has the session key to produce a valid auth tag. Channel: Source encrypted inside ciphertext with channel key — only channel members can verify. |
| Spoofed beacons | Beacons include a 4-byte truncated HMAC (§4.9) keyed with pairwise DM session key. Beacons from known peers are authenticated; unknown peers' beacons are flagged lower-trust. A malicious beacon from an unknown node can claim false battery/queue/time, but its influence is limited (especially for time sync — see §9.3 corroboration requirements). |
| Spoofed routing | RREP packets include an 8-byte truncated HMAC (§4.7) keyed with static DH shared secret. Authenticated RREPs are trusted; unauthenticated RREPs (first-contact) create "unverified" routes that are promoted only after KEY_EXCHANGE. Fabricated routes without valid HMAC are rejected. |

#### 10.2.3 Privacy

| Threat | Protection | Residual Risk |
|--------|-----------|---------------|
| Identify who talks to whom (DM) | RREQ source encrypted. Route-based forwarding reveals path only to nodes on the path. | Relay nodes see traffic patterns (timing, frequency, volume) between next-hop pairs. Local passive attacker can correlate. |
| Identify channel message sender | Source addr encrypted inside channel ciphertext. Wire shows `0x00000000`. Channel ID also inside ciphertext — non-members cannot determine which channel. | Channel members see the sender. Non-member relay nodes cannot identify sender or channel. |

**Channel sender privacy note:** Channel membership inherently implies sender visibility to all members. Channel security is only as strong as the least trustworthy member — any member can read all messages, identify all senders, and potentially leak channel content. Users requiring sender anonymity should use DMs. Channels are designed for group communication where mutual trust among members is assumed.
| Track node movement | Node address is derived from public key — persistent across locations. | An observer who recognizes a node's address can track it. Mitigation: address rotation (future enhancement). |
| Enumerate network size/topology | No global topology broadcast. Beacons are local (1-hop). | A patient passive attacker can infer topology from traffic patterns over time. |

### 10.3 Metadata Leakage Analysis

**What a passive observer learns from a single captured packet:**

| Packet Type | Observable Metadata |
|-------------|-------------------|
| DATA (DM) | dest_addr, next_hop, packet_id, packet size, tier, app_type. NOT: source, content. |
| DATA (Channel) | next_hop, packet_id, packet size. NOT: source, content, dest, channel_id (inside ciphertext). |
| ACK | src_addr (ACK sender = original destination), ack_packet_id. NOT: original source. |
| RREQ | dest_addr (who is being searched for), query_id. NOT: who is searching. |
| RREP | src_addr (who was found), query_id. NOT: who was searching. |
| BEACON | src_addr, pubkey_hash, uptime, battery, queue depth, neighbor count, time. |

**Key exposure:** A passive attacker monitoring a relay node sees `dest_addr` on DM packets. This reveals that *someone* is communicating with `dest_addr`. The source remains hidden from relay nodes. A global passive adversary observing all nodes could correlate RREQ timing with subsequent DATA flows to identify source-destination pairs.

### 10.4 Replay Attack Mitigation

```
Protection layers:
1. Packet ID dedup buffer (256 entries, 60-second window)
   → Prevents exact packet replay within 60 seconds
2. AES-GCM nonce (96-bit, never reused per key)
   → Replay of encrypted content is detected by nonce tracking
3. Timestamp validation (±30 second window)
   → Old packets outside window are rejected
4. Combined: An attacker must replay within 30 seconds, with a never-seen packet_id,
   and a valid nonce — effectively impossible.
```

### 10.5 Flood/DoS Mitigation

```
Protection layers:
1. Airtime budget — each node self-limits to 10% duty cycle
   → A compromised node can waste its own airtime but not others'
2. RREQ rate limiting — max 1 RREQ per destination per 30 seconds per neighbor
   → Prevents RREQ flooding
3. Beacon rate limiting — beacons from same source more than 1/30s are dropped
   → Prevents beacon storms
4. Packet dedup — duplicate packets silently dropped
   → Amplification attacks are ineffective
5. TX queue priority — flooding with low-priority packets doesn't affect critical traffic
```

```
function rreq_rate_check(source_neighbor, dest_addr, query_id):
    key = hash(source_neighbor, dest_addr)
    last = rreq_rate_table.lookup(key)
    
    if last != NULL and (now() - last.timestamp) < 30:
        drop("RREQ rate limited")
        return false
    
    rreq_rate_table.upsert(key, now())
    return true
```

RREQ rate table: max 64 entries × 12 bytes = 768 bytes.

### 10.6 Sybil Attack Mitigation

A Sybil attack (one attacker creating many fake node identities) is partially mitigated:

1. **Computational cost:** Each identity requires an X25519 key pair. Generating many is cheap (ESP32 does ~50/sec), but *using* them requires separate radio transmissions for each.
2. **Airtime cost:** Each Sybil identity must transmit its own beacons, consuming attacker's airtime. With 10% duty cycle per identity, one radio can support ~3-4 active Sybil identities before airtime exhaustion.
3. **Neighbor suspicion:** Nodes track per-neighbor behavior metrics. If one physical neighbor produces packets from many identities (indicated by identical RSSI/timing patterns), a heuristic flags them:

```
function neighbor_sybil_check():
    // Group neighbors by similar RSSI (within ±3 dB)
    groups = cluster_neighbors_by_rssi(threshold=3)
    
    for group in groups:
        if len(group) > 3:
            // Suspicious — 4+ nodes at nearly identical RSSI
            // Flag all but the oldest (highest uptime) as suspicious
            for node in group.sorted_by_uptime()[3:]:
                neighbor_table.set_suspicious(node.addr, true)
                // Reduce trust: don't use suspicious nodes as next-hop
```

This is a heuristic, not a guarantee. Bramble acknowledges that Sybil resistance on a permissionless network is an open problem. The protocol is designed to degrade gracefully under Sybil attack (routing becomes less efficient but communication continues via alternate paths).

### 10.7 Compromised Node Containment

If a node is compromised:
- **DM keys:** Only DM sessions involving that node are exposed. Other pairs' sessions are safe.
- **Channel keys:** If the compromised node is in a channel, that channel's content is exposed. Other channels are safe.
- **Routing:** A compromised relay node can drop, delay, or misroute packets. Mitigation: senders detect via missing ACKs and discover alternate routes.
- **Exclusion:** There is no mechanism to revoke a node identity in a decentralized mesh. Compromised nodes must be excluded out-of-band (e.g., rotate channel PSKs, stop sending DMs to the compromised address).

### 10.8 Additional Security Hardening

**Dummy traffic ("privacy mode"):** When enabled, nodes generate dummy DATA packets indistinguishable from real encrypted traffic. Dummy packets use random destinations, valid-looking encrypted payloads, and are transmitted at random intervals (5–30 seconds) within a 2% airtime budget. This defeats traffic analysis attacks that rely on transmission timing or frequency to infer communication patterns.

**Constant-time channel trial decryption:** Channel message decryption always attempts all 16 channel keys regardless of which key (if any) succeeds. This prevents timing side-channel attacks where an observer could determine which channel a message belongs to by measuring decryption time.

**Nonce counter persistence:** AES-GCM nonce counters are periodically persisted to NVS to ensure nonce uniqueness survives unexpected reboots. On startup, the counter is restored from NVS and advanced by a safety margin to account for any unsaved increments. This prevents nonce reuse (which would catastrophically break AES-GCM security) after power loss.

### 10.9 Security Considerations for New Features

#### Mailbox / Store-and-Forward Security

| Threat | Mitigation |
|--------|-----------|
| Mailbox buffer flooding | Per-destination cap (8 entries), per-source cap (8 entries) |
| Spoofed STORE_REQUEST | HMAC authentication with pairwise session key |
| Content inspection by mailbox | Messages are E2E encrypted — mailbox sees only ciphertext |
| Mailbox denying service | Sender tries multiple mailbox neighbors; falls back to retry on destination return |
| Replay of stored messages | Original `packet_id` preserved; destination dedup rejects replays |
| TTL abuse for storage exhaustion | Mailbox caps TTL to `min(requested, 24h)` |

**Residual risk:** Mailbox node learns that source is trying to reach destination (traffic analysis). Acceptable tradeoff for offline delivery functionality.

#### Emergency Beacon Security

The emergency component was removed unshipped (section 4.19); there is no emergency traffic on the air.

#### Private Location Sharing Security

| Threat | Mitigation |
|--------|-----------|
| Location leaked to relay nodes | Location payloads are E2E encrypted inside DM packets |
| Recipient sharing location with others | Social/trust issue, not protocol-level — same as any private data |
| Stale location data | 1-hour cache TTL; `location_cache_purge()` evicts expired entries |
| Update flooding | Time-based (5 min default) and distance-based (100m) triggers; low priority in TX queue |

**Privacy tiers rationale:**
- FULL (17 bytes): Trusted contacts only — hiking partners, emergency contacts
- COARSE (5 bytes): Casual contacts — "which neighborhood are you in"
- PRESENCE (1 byte): Acquaintances — "are you online"

#### Group DM Security

The group-DM component was removed unshipped (section 5.3); there is no group traffic on the air.

#### Network Coding Security

The network-coding component was removed unshipped (section 4.21); no coded packets exist on the air.

#### Broadcast Probe Security

| Threat | Mitigation |
|--------|-----------|
| Probe flooding | Rate limiting: 3 tokens, 1 refill/minute; cannot sustain more than 3/min |
| Fake probe responses | Responses tied to observed probe_id; spoofer must have heard actual probe |
| Network enumeration | Probes are visible to all nodes — inherent design. Use sparingly. |
| Response amplification | ACK jitter (100–2000ms) spreads responses; dedup prevents re-processing |

**Design note:** Broadcast probes are intentionally rare (3/min max). They exist for network health diagnostics, not routine operation.

#### Public Channel Security

| Threat | Mitigation |
|--------|-----------|
| Content confidentiality | **None** — well-known PSK means anyone can decrypt. By design. |
| Spam flooding | Broadcast-tier airtime budget at the TX chokepoint (section 8.2.1). A dedicated public-channel TX/RX rate limiter was removed unshipped. |
| Impersonation | Source address is identity-derived; spoofer must know victim's private key |

**Warning:** Public Channel provides **no confidentiality**. It exists for community broadcast and new-node introduction. Use private channels or DMs for sensitive content.

---

## 11. Resource Budget

### 11.1 RAM Budget

```
Subsystem                      Size        Notes
────────────────────────────   ─────────   ─────────────────────────
Routing table                  1,536 B     64 entries × 24 B
Neighbor table                   640 B     32 entries × 20 B
Pending route discoveries        160 B     8 entries × 20 B
RREQ dedup cache               1,024 B     128 entries × 8 B
Reverse route table               384 B     32 entries × 12 B
Packet dedup buffer             2,048 B     256 entries × 8 B
RX buffer                         256 B     1 packet being processed
Pending ACK table               1,888 B     8 entries × 236 B
Fragment reassembly             2,464 B     4 concurrent × 4 frags × 154 B
DM session key cache            2,048 B     32 entries × 64 B (32B key + 32B metadata)
Peer public key cache           2,048 B     64 entries × 32 B
Airtime budget state               32 B     Token bucket state
Time sync state                    20 B     Sync state
RREQ rate limit table             768 B     64 entries × 12 B
Mailbox buffer                  7,040 B     32 entries × 220 B (204B payload + 16B metadata)
Location manager                1,088 B     16 contacts × 68 B (config + cache)
Probe state                       648 B     32 responses × 20 B + control state
────────────────────────────────────────────────────────────────
Protocol subtotal:             25,532 B     (~25 KB)

Application buffers:
  Display framebuffer           1,024 B     128×64 / 8
  Serial/BLE RX buffer           512 B     User input
  Application message buffer    1,400 B     Max reassembled message
  String/UI buffers             2,048 B     Menu, status text
────────────────────────────────────────────────────────────────
Application subtotal:           4,984 B     (~5 KB)

FreeRTOS overhead:
  Task stacks (4 tasks × 4KB)  16,384 B    Radio, protocol, app, idle
  Kernel objects                 2,048 B    Queues, semaphores, timers
────────────────────────────────────────────────────────────────
RTOS subtotal:                 18,432 B     (~18 KB)

ESP-IDF system:
  WiFi/BLE stack (if enabled)  ~65,536 B    ~64 KB (BLE only; WiFi disabled in mesh mode)
  Heap management               ~8,192 B    ~8 KB
  NVS cache                     ~4,096 B    ~4 KB
  Miscellaneous                 ~8,192 B    ~8 KB
────────────────────────────────────────────────────────────────
System subtotal:               ~86,016 B    (~84 KB)

════════════════════════════════════════════════════════════════
TOTAL RAM ESTIMATE:           ~142,916 B    (~140 KB)
Available on ESP32-S3:        ~327,680 B    (~320 KB)
Headroom:                     ~184,764 B    (~180 KB, 56% free)
```

The 60% headroom accommodates:
- Heap fragmentation (10–15%)
- BLE connection buffers when active (~20KB)
- Temporary crypto buffers (X25519, HKDF, AES-GCM working memory)
- Unexpected growth in routing tables during network events

### 11.2 Flash Budget

```
Component                      Size         Notes
────────────────────────────   ──────────   ─────────────────────────
Bramble firmware               ~256 KB      Protocol + application code
ESP-IDF system                 ~512 KB      Kernel, drivers, BLE stack
Bootloader                      ~32 KB      ESP-IDF bootloader
Partition table                   4 KB      Standard
NVS partition                    20 KB      Keys + config
OTA partition (×2)             ~768 KB      Two slots for OTA update
SPIFFS/LittleFS                ~200 KB      Web UI assets, logs
────────────────────────────────────────────────────────────────
TOTAL:                        ~1,792 KB     (~1.75 MB)
Available:                     4,096 KB     (4 MB standard)
Headroom:                     ~2,304 KB     (~2.25 MB, 56% free)
```

### 11.3 Data Structure Limits and Eviction

All tables have hard size limits. When a table is full, the eviction policy determines what is removed:

```
Table                    Max    Eviction Policy
────────────────────     ────   ──────────────────────────────
Routing table            64     LRU by last_used timestamp. Stale evicted before active.
Neighbor table           32     LRU by last_heard. Neighbors not heard for >600s evicted first.
RREQ dedup               128    Time-based: entries >60s are purged on every insert.
Reverse routes           32     Time-based: entries >60s are purged.
Packet dedup             128    Time-based: entries >60s are purged.
Pending ACKs             8      Oldest entry dropped (after sending failure notification).
Fragment reassembly      4      Oldest incomplete reassembly dropped.
Key cache                32     LRU by last-use. Evicted keys must be re-exchanged.
Peer pubkey cache        64     LRU. Evicted entries relearned from beacons.
RREQ rate table          64     LRU by timestamp.
```

---

## 12. Implementation Roadmap

### Phase 1: Core Protocol (Weeks 1–6)

**Goal:** Two nodes communicating reliably over LoRa with routing.

**Week 1–2: Foundation**
- ESP-IDF project setup with FreeRTOS task structure
- SX1262 driver: TX, RX, CAD, RSSI/SNR reading
- Packet serialization/deserialization for all types (§4)
- Packet dedup buffer
- Serial console for debugging

**Week 3–4: Identity & Crypto**
- X25519 key pair generation and NVS storage (§5.1)
- AES-256-GCM encrypt/decrypt using ESP32 hardware acceleration (mbedtls)
- Channel PSK derivation and storage (§5.3)
- Key exchange protocol: initiate, respond, confirm (§5.2)
- DM encryption/decryption with session keys

**Week 5–6: Routing & Forwarding**
- Neighbor table: population from received packets (RSSI, SNR tracking)
- Beacon generation and processing (§4.9)
- RREQ/RREP route discovery with privacy-preserving source encryption (§6.3)
- Routing table: install, lookup, timeout, eviction
- Data forwarding along routes (§6.6)
- Route error detection and propagation (§6.4)

**Deliverable:** Two ESP32 nodes can discover each other, exchange keys, and send encrypted DMs via a third relay node. Routes are cached and reused.

### Phase 2: Reliability & Airtime (Weeks 7–10)

**Week 7–8: Reliability Layer**
- Three-tier message model (§7.1)
- ACK generation and matching (§7.2)
- Retry engine with exponential backoff (§7.3)
- Pending ACK table and timeout management
- Delivery receipts with relay path building (§7.2)
- Sliding window flow control (§7.5)

**Week 9–10: Airtime & Scheduling**
- Token bucket airtime budget (§8.2)
- Airtime calculation function (§8.3)
- Priority TX queue with scheduling (§8.5)
- Congestion detection and response (§8.4)
- Congestion packet generation and processing
- Listen-before-talk with CAD (§3.3)

**Deliverable:** Messages reliably delivered across multi-hop paths with congestion awareness. Critical messages survive temporary link outages via retries.

### Phase 3: Time Sync & Security Hardening (Weeks 11–13)

**Week 11–12: Time Synchronization**
- Beacon-based time sync (§9.2, §9.3)
- TIME_SYNC packet emission for low-stratum nodes (§9.5)
- Stratum tracking and source selection
- Anti-replay timestamp validation (§9.6)
- Optional GPS time source integration (for T-Beam)

**Week 13: Security Hardening**
- RREQ rate limiting (§10.5)
- Sybil detection heuristic (§10.6)
- Nonce tracking for AES-GCM (prevent reuse)
- Key rotation triggers (time-based and count-based, §5.5)
- Packet validation: malformed packet rejection, hop limit enforcement

**Deliverable:** Full protocol operational with time sync, anti-replay, and attack mitigations.

### Phase 4: Fragmentation & Channel Messages (Weeks 14–16)

**Week 14: Fragmentation**
- Fragment header serialization (§4.14)
- Fragment transmission: splitting messages >172 bytes
- Reassembly engine: bitmap tracking, timeout, dedup
- Integration with reliability layer (retry individual fragments)

**Week 15–16: Channel (Group) Messages**
- Channel message encryption with PSK (§4.4, §5.3)
- Controlled flood for channel messages (§6.7)
- Channel join/leave (local config only, no protocol negotiation)
- Multi-channel support (up to 16 simultaneous)

**Deliverable:** Full-featured messaging: DMs with E2E encryption, channels with PSK, long messages via fragmentation.

### Phase 5: User Interface (Weeks 17–20)

**Week 17–18: OLED Display & Buttons**
- Main screen: node info, battery, neighbor count, time
- Message list screen: recent messages with sender, timestamp
- Compose screen: canned messages + character input via buttons
- Settings screen: channel management, TX power, radio profile
- Node list screen: known peers with signal quality

**Week 19–20: BLE Interface**
- BLE GATT service for phone companion app communication
- Message send/receive over BLE
- Configuration read/write over BLE
- Node list and routing table inspection
- Real-time status updates (push notifications)

**Deliverable:** Usable standalone device with OLED UI. Phone connectivity via BLE for richer interaction.

### Phase 6: Web Firmware Flasher (Weeks 21–23)

**Goal:** Browser-based firmware flashing — no IDE, no toolchain, no CLI.

**Week 21: Web Serial API Flasher**
- Static web app (HTML/JS, no backend) hosted on GitHub Pages or similar
- Uses Web Serial API to connect to ESP32 via USB
- Implements ESP32 serial bootloader protocol (esptool.js or custom)
- Firmware binary hosted alongside the web page (or fetched from GitHub Releases)
- Flashing flow:
  1. User connects ESP32 via USB
  2. Opens web page in Chrome/Edge (Web Serial requires Chromium)
  3. Clicks "Connect" → selects serial port
  4. Firmware binary downloaded/cached
  5. ESP32 put into bootloader mode (automatic via RTS/DTR, or manual button)
  6. Flash progress bar
  7. Reboot into new firmware

**Week 22–23: Flasher Polish**
- Firmware version detection (read current version before flashing)
- Multi-board support (detect Heltec V3 vs T-Beam via USB VID/PID or user selection)
- Partition table and bootloader flashing for first-time setup
- NVS preservation during upgrade (flash only app partitions)
- Release management: GitHub Actions CI builds firmware, publishes to Releases, web flasher fetches latest

**Deliverable:** Non-technical users can flash Bramble firmware from a web browser with zero toolchain setup. URL: `bramble.example.com/flash`

### Phase 7: Config & Messaging Web App (Weeks 24–28)

**Goal:** Browser-based configuration and messaging interface connected via Web Serial or BLE.

**Week 24–25: Web Serial Configuration App**
- Single-page web app (Svelte or vanilla JS, minimal bundle)
- Connects to Bramble device via Web Serial API
- Serial protocol: JSON-RPC over UART at 115200 baud
  - `{"method": "get_config", "id": 1}` → `{"result": {...}, "id": 1}`
  - `{"method": "set_config", "params": {...}, "id": 2}`
  - `{"method": "send_message", "params": {"dest": "...", "text": "..."}, "id": 3}`
  - `{"method": "get_messages", "params": {"since": 12345}, "id": 4}`
  - `{"method": "get_nodes", "id": 5}`
  - `{"method": "get_routes", "id": 6}`
- Configuration pages:
  - Node identity (view public key, address)
  - Radio settings (profile, TX power, frequency)
  - Channel management (add/remove/rename channels, set PSK)
  - Peer management (view known peers, signal quality, routes)
  - Airtime stats (budget usage, congestion level)

**Week 26–27: Messaging Interface**
- Chat-style message interface per DM contact and per channel
- Message compose with send tier selection (Broadcast/Normal/Critical)
- Delivery status indicators (sent → delivered → read)
- Relay path display on delivered messages (from delivery receipts)
- Node map view (if position data available from peers)
- Real-time updates via serial polling (every 2 seconds) or push (serial interrupt)

**Week 28: Web BLE Alternative & OTA Updates**
- Web Bluetooth API support as alternative to Web Serial
- Same JSON-RPC protocol over BLE GATT characteristic
- Enables phone browser access (Chrome Android supports Web Bluetooth)
- Connection manager: auto-reconnect, connection status indicator
- **BLE OTA firmware update** using ESP-IDF BLE OTA support — enables firmware updates from phone/laptop without USB
- **WiFi OTA** as optional convenience (when WiFi available) — ESP-IDF native HTTP OTA
- **LoRa OTA explicitly not planned** — airtime cost is prohibitive (256KB firmware ≈ 2.4 hours of continuous airtime at SF10/125kHz, exhausting weeks of airtime budget)

**Deliverable:** Full-featured web app for configuring and using Bramble devices from any Chromium browser. Works via USB (Web Serial) or wireless (Web Bluetooth). BLE OTA enables field firmware updates without physical access. No native app installation required.

### Phase 8: Testing & Optimization (Weeks 29–32)

**Week 29–30: Automated Testing**
- Unit tests for all packet serialization/deserialization
- Unit tests for crypto operations (known test vectors)
- Integration tests: simulated multi-node mesh on a single ESP32 (loopback radio mock)
- Replay attack test suite
- Congestion scenario simulation (max queue depth, budget exhaustion)

**Week 31–32: Field Testing & Optimization**
- 5-node field test: range, reliability, latency measurements
- 20+ node stress test: routing convergence time, airtime distribution
- Power consumption profiling: active, idle, deep sleep modes
- Memory high-water-mark analysis under sustained load
- Flash wear analysis for NVS writes (key rotation frequency)

**Deliverable:** Validated, optimized firmware ready for broader deployment. Published test results and performance benchmarks.

---

## Appendix A: Cryptographic Primitive Summary

| Primitive | Algorithm | Key Size | Library | ESP32 HW Accel |
|-----------|-----------|----------|---------|----------------|
| Key exchange | X25519 | 256-bit | mbedtls (bundled in ESP-IDF) | No (software, ~5ms on ESP32-S3) |
| Symmetric encryption | AES-256-GCM | 256-bit | mbedtls | Yes (ESP32-S3 AES accelerator) |
| Key derivation | HKDF-SHA256 | N/A | mbedtls | SHA-256 HW accelerated on ESP32-S3 |
| Hashing | SHA-256 | N/A | mbedtls | Yes (ESP32-S3 SHA accelerator) |
| Authentication | HMAC-SHA256 | 128-bit | mbedtls | Via SHA-256 HW |
| Random numbers | Hardware RNG | N/A | esp_random() | Yes (true hardware RNG) |

**AES-GCM nonce management:**
- 96-bit (12-byte) nonce per packet
- Constructed as: `nonce = src_addr (4B) || packet_counter (4B) || random (4B)`
- The `packet_counter` is a per-session monotonic counter (stored in RAM, not NVS)
- This construction guarantees nonce uniqueness as long as the session key is not used with >2³² packets (enforced by the 65,536-message rekey trigger, well below this limit)

## Appendix B: Configuration Defaults

```
// Radio
radio_profile        = LongRange           // SF10, 125kHz, CR 4/6
tx_power_dbm         = 22                  // Max legal
frequency_mhz        = 906.875

// Routing
max_hop_limit        = 8
route_active_timeout  = 300                // seconds
route_stale_timeout   = 600                // seconds
route_hard_timeout    = 3600               // seconds
max_routing_entries   = 64
max_neighbors         = 32

// Reliability
normal_max_retries    = 3
critical_max_retries  = 8
normal_base_delay_ms  = 2000
critical_base_delay_ms = 3000
ack_timeout_ms        = 5000               // Time to wait for ACK before retry
window_size           = 4

// Airtime
duty_cycle_percent    = 10
critical_budget_pct   = 40
normal_budget_pct     = 40
broadcast_budget_pct  = 20

// Beacons
beacon_interval_s     = 60                 // Beacon every 60 seconds
beacon_jitter_s       = 10                 // ±10s random jitter

// Time sync
sync_interval_s       = 300                // TIME_SYNC emission (stratum ≤ 2 only)
replay_window_s       = 30

// Channels
max_channels          = 16
channel_default_hops  = 3                  // Hop limit for channel messages

// Fragmentation
max_fragments         = 4
fragment_timeout_ms   = 30000              // 30 seconds to reassemble
max_concurrent_reasm  = 4

// Privacy & Security
allow_open_rreq       = false              // Default: contacts-only mode (encrypted RREQ source)
                                           // Set true to allow first-contact with plaintext source

// Channel epoch ratchet
channel_epoch_hours   = 24                 // Hours between automatic epoch advances
channel_epoch_messages = 256               // Messages before forced epoch advance (whichever comes first)
```

---

## 13. Future Enhancements

The following features are out of scope for the initial implementation but are tracked for future consideration:

### 13.1 Dual-Channel Control/Data Split

Split control traffic (beacons, RREQ/RREP, ACKs) and data traffic onto separate frequencies. The SX1262 supports frequency switching in ~100µs, making rapid alternation feasible. Control channel would use a fixed frequency with lighter traffic; data channel handles bulk message delivery. This would approximately double effective throughput and reduce control/data contention.

### 13.2 Address Rotation for Location Privacy

Periodically rotate node addresses to prevent long-term tracking by passive observers. Rotation requires coordinating with active DM peers (notify of new address) and re-announcing via beacon. Tradeoffs include increased routing churn and complexity in key-to-address mapping. Design must balance privacy benefit against routing disruption cost.

### 13.3 LoRa Frequency Survey Tooling

Diagnostic mode that scans the ISM band (902–928 MHz) and reports noise floor, interference sources, and signal quality per sub-band. Useful for deployment planning — identifying the best operating frequency for a specific geographic area. Could run as a standalone firmware mode or integrated diagnostic tool.

### 13.4 Companion Phone App

Native mobile application (iOS/Android) for richer interaction beyond Web BLE capabilities. Would provide background BLE connectivity, push notifications for incoming messages, persistent message storage, and contact management. Web BLE (Phase 7) serves as the MVP; a native app would offer better reliability and UX for daily use.

---

## Appendix C: Glossary

| Term | Definition |
|------|-----------|
| **AODV** | Ad-hoc On-demand Distance Vector — reactive routing protocol that discovers routes only when needed |
| **CAD** | Channel Activity Detection — LoRa radio feature to detect ongoing transmissions |
| **CR** | Coding Rate — Forward Error Correction ratio in LoRa (4/5 through 4/8) |
| **DH** | Diffie-Hellman — key exchange protocol; X25519 is an elliptic curve variant |
| **E2E** | End-to-End — encryption where only sender and receiver can decrypt |
| **GCM** | Galois/Counter Mode — authenticated encryption mode for AES |
| **HKDF** | HMAC-based Key Derivation Function — derives cryptographic keys from shared secrets |
| **LBT** | Listen Before Talk — checking channel is clear before transmitting |
| **NVS** | Non-Volatile Storage — ESP32 flash-based key-value store |
| **OTP** | One-Time Pad — XOR-based encryption (used in compact RREQ source encryption) |
| **PSK** | Pre-Shared Key — symmetric key distributed out-of-band |
| **RERR** | Route Error — notification that a route is broken |
| **RREP** | Route Reply — response to a route discovery request |
| **RREQ** | Route Request — broadcast query to discover a route to a destination |
| **SF** | Spreading Factor — LoRa modulation parameter (SF7–SF12); higher = longer range, slower speed |
| **SNR** | Signal-to-Noise Ratio — quality metric for received signals (dB) |
| **Stratum** | Time sync hierarchy level — lower = closer to reference clock |
| **X25519** | Elliptic curve Diffie-Hellman using Curve25519 — fast, secure key exchange |

---

## Revision History

| Date | Changes |
|------|---------|
| 2026-02-15 | Initial design document (v0.1-draft) |
| 2026-02-16 | **Adversarial review fixes:** Dedup buffer 128→256 entries. RREP HMAC 4→8 bytes (RREP_SIZE 30→34). RREQ salt field added (RREQ_SIZE 26→30) to prevent temporal correlation. Max fragments 8→4 (max reassembled 616B). Long-term pubkey added to KEY_EXCHANGE (69→101 bytes). KEY_EXCHANGE now Critical-tier (8 retries). Nonce counter persistence for reboot safety. Constant-time channel trial decryption. TX power reduction removed from congestion response (replaced with increased backoff). **New features:** NVS key backup via BLE with physical button authorization. Dummy traffic generation (privacy mode). Route advertisements in beacons. |
| 2026-02-17 | **Packet type renumbering:** Types renumbered to match implementation (ACK=0x01 through DATA=0x0A, see §4.3). **New packet types (§4.15–4.23):** STORE_REQUEST (0x0B), STORE_ACK (0x0C), MAILBOX_DELIVERY (0x0D), MAILBOX_QUERY (0x0E) for store-and-forward; EMERGENCY (0x0F), EMERGENCY_CANCEL (0x10) for distress signaling; CODED (0x11) for XOR network coding; PKT_TYPE_PROBE (0x12), PKT_TYPE_PROBE_ACK (0x13) for delivery tracking. **Beacon flag extensions (§4.9):** BEACON_FLAG_MAILBOX (0x01), BEACON_FLAG_PROBE_ACK (0x02). **Header flag extension:** HEADER_FLAG_EMERGENCY (0x04) for emergency relay priority. **New channel features (§5.3):** Public channel "Bramble Common" with well-known PSK; Group DM key derivation with FNV-1a and epoch rotation. **Private location sharing (§4.24):** Three-tier privacy (full/coarse/presence), 17-byte full format, per-contact config. **Adaptive routing metrics (§6.8):** Composite metric with delivery rate, airtime, latency factors; EMA tracking; hysteresis. **Security analysis (§10.9):** New section covering security properties of all new features. **RAM budget update (§11.1):** Added ~13 KB for new components (mailbox 7KB, emergency 0.4KB, location 1KB, group 2.2KB, coding 1.5KB, probe 0.6KB). |
