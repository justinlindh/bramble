# Network Working Group                                           Bramble Project
Internet-Draft                                          Implementation Snapshot
Intended status: Informational                                   2 March 2026
Expires: 3 September 2026

# Bramble Mesh Protocol (Protocol Version 0.5.0, Wire Version 1)

## Abstract

This document specifies the Bramble mesh protocol wire format and node behavior
for ESP32-class LoRa devices. Bramble defines a compact binary packet format,
reactive route discovery, reliability tiers, encrypted direct and channel
messages, bounded fragmentation, and beacon-based time synchronization.

This document is an implementation-aligned Internet-Draft style specification
based on the current Bramble protocol specification and firmware constants.

## Status of This Memo

This Internet-Draft is submitted for documentation and interoperability within
the Bramble ecosystem. It is not an IETF standards-track document.

The protocol/API version reflected by this draft is **0.5.0** and the wire
version is **1** (`BRAMBLE_VERSION = 1`).

This snapshot predates the June 2026 wire-or-delete reckoning: packet type
codes 0x08 (CONGESTION), 0x09 (TIME_SYNC), 0x0F (EMERGENCY), 0x10
(EMERGENCY_CANCEL), and 0x11 (CODED) referenced below have since been retired
unshipped, and `HEADER_FLAG_EMERGENCY` no longer exists.
[bramble-protocol-spec.md](bramble-protocol-spec.md) is the current
description.

## Table of Contents

1.  [Conventions and Terminology](#1-conventions-and-terminology)
2.  [Protocol Overview](#2-protocol-overview)
3.  [Physical Layer and Airtime Behavior](#3-physical-layer-and-airtime-behavior)
4.  [Common Packet Header](#4-common-packet-header)
5.  [Packet Type Registry](#5-packet-type-registry)
6.  [Packet Formats](#6-packet-formats)
7.  [Fragmentation and Reassembly](#7-fragmentation-and-reassembly)
8.  [Identity and Key Management](#8-identity-and-key-management)
9.  [Routing Protocol](#9-routing-protocol)
10. [Reliability Model](#10-reliability-model)
11. [Time Synchronization](#11-time-synchronization)
12. [Resource Constraints and Limits](#12-resource-constraints-and-limits)
13. [Security Considerations](#13-security-considerations)
14. [IANA Considerations](#14-iana-considerations)
15. [References](#15-references)
16. [Implementation Snapshot Notes](#16-implementation-snapshot-notes)

## 1. Conventions and Terminology

The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**,
**SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **NOT RECOMMENDED**, **MAY**, and
**OPTIONAL** in this document are to be interpreted as described in BCP 14
(RFC 2119, RFC 8174) when, and only when, they appear in all capitals.

All multi-byte integers are network byte order (big-endian).

Node addresses are 32-bit values derived from public keys.

## 2. Protocol Overview

Bramble is a LoRa mesh protocol with:

* A 12-byte fixed common header;
* Enumerated packet families for routing, reliability, congestion, sync,
  encrypted data, and operational extensions;
* Reactive (AODV-inspired) route discovery with RREQ/RREP/RERR;
* Three reliability tiers (broadcast, normal, critical);
* End-to-end AES-256-GCM encryption for direct and channel traffic;
* Bounded fragmentation for larger payloads;
* Beacon and TIME_SYNC-based time distribution.

Nodes are constrained embedded devices. Protocol behavior therefore prioritizes
bounded memory growth, fixed table limits, and conservative airtime use.

## 3. Physical Layer and Airtime Behavior

### 3.1 Radio Profiles

All nodes in a mesh MUST use a common profile.

**LongRange (default):** 906.875 MHz, SF10, BW 125 kHz, CR 4/6, preamble 12,
sync word 0x1424, explicit header, CRC enabled, max payload 222 bytes.

**MediumRange:** 906.875 MHz, SF8, BW 250 kHz, CR 4/5, preamble 8.

### 3.2 Listen-Before-Talk (CAD/LBT)

Before transmission, nodes MUST perform Channel Activity Detection (CAD):

* `LBT_MAX_ATTEMPTS = 3`
* `LBT_BACKOFF_BASE_MS = 50`
* `LBT_BACKOFF_MAX_MS = 300`

If channel activity remains detected after max attempts, nodes MAY transmit
anyway (anti-starvation behavior).

### 3.3 Broadcast Receipt Collision Avoidance

For broadcast delivery receipts, Bramble uses deterministic slotting plus jitter
and retries:

* `SLOT_BUCKETS = 32`
* `SLOT_SPACING_MS = 200`
* `SLOT_BASE_MS = 200`
* `RECEIPT_RETRY_COUNT = 3`

Nodes MUST compute receipt slot from `(local_addr XOR original_packet_id) %
SLOT_BUCKETS`. Nodes MUST still run LBT before each receipt attempt.

### 3.4 Airtime Calculation and Budgeting

Airtime SHOULD be computed per Semtech LoRa equations using configured SF/BW/CR.
At LongRange defaults, reference airtime is approximately:

* 22-byte ACK: ~290 ms
* 36-byte beacon: ~400 ms
* 100-byte payload: ~480 ms
* 200-byte payload: ~850 ms

The protocol specification defines a token-bucket model with 10% self-imposed
airtime budget and per-tier sub-buckets (critical 40%, normal 40%, broadcast
20%). Implementations SHOULD preserve this tier partitioning.

## 4. Common Packet Header

All packets start with a fixed 12-byte header:

```
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +---------------+---------------+---------------+---------------+
 |    Version    |      Type     |     Flags     |   Hop Limit   |
 +---------------+---------------+---------------+---------------+
 |                          Destination Address                  |
 +---------------------------------------------------------------+
 |                           Packet ID                           |
 +---------------------------------------------------------------+
```

Fields:

* Version: wire version (`1`)
* Type: packet type registry value
* Flags: tier/ack/receipt/channel/encrypt/fragment bitfield
* Hop Limit: decremented at each relay; packet dropped at 0
* Destination Address: 32-bit destination (`0xFFFFFFFF` for broadcast)
* Packet ID: 32-bit dedup/reliability key

### 4.1 Flags Layout

```
  7   6   5       4        3        2        1   0
 +---+---+-------+--------+--------+--------+---+---+
 |T1 |T0 |ACK_REQ|RECEIPT |CHANNEL |ENCRYPT |F1 |F0 |
 +---+---+-------+--------+--------+--------+---+---+
```

* `T1:T0`: reliability tier (00 broadcast, 01 normal, 10 critical, 11 reserved)
* `ACK_REQ`: end-to-end ACK requested
* `RECEIPT`: delivery receipt requested
* `CHANNEL`: channel/group semantics for DATA path
* `ENCRYPT`: encrypted payload indicator
* `F1:F0`: fragmentation state bits

For emergency packet classes, bit 2 is repurposed as `HEADER_FLAG_EMERGENCY`.

## 5. Packet Type Registry

| Value | Name |
|---|---|
| 0x01 | ACK |
| 0x02 | ROUTE_REQUEST (RREQ) |
| 0x03 | ROUTE_REPLY (RREP) |
| 0x04 | ROUTE_ERROR (RERR) |
| 0x05 | BEACON |
| 0x06 | KEY_EXCHANGE |
| 0x07 | DELIVERY_RECEIPT |
| 0x08 | CONGESTION |
| 0x09 | TIME_SYNC |
| 0x0A | DATA |
| 0x0B | STORE_REQUEST |
| 0x0C | STORE_ACK |
| 0x0D | MAILBOX_DELIVERY |
| 0x0E | MAILBOX_QUERY |
| 0x0F | EMERGENCY |
| 0x10 | EMERGENCY_CANCEL |
| 0x11 | CODED |
| 0x12 | PKT_TYPE_PROBE |
| 0x13 | PKT_TYPE_PROBE_ACK |
| 0x14 | LOCATION |

## 6. Packet Formats

This section defines wire layouts. The common 12-byte header appears first in
all packet formats unless explicitly noted.

### 6.1 ACK (0x01)

Minimum wire size 23 bytes; maximum 55 bytes.

```
 +------------------------------- Header (12) -------------------------------+
 |                             Source Address                                |
 +---------------------------------------------------------------------------+
 |                           Acked Packet ID                                 |
 +---------------+---------------+-------------------------------------------+
 |   Ack Flags   | RSSI@Dest+128 | Hop Count (N)                             |
 +---------------+---------------+-------------------------------------------+
 |                        Relay Path[0] (optional)                           |
 +--------------------------------------------------------------- ... -------+
```

Relay path is up to 8 entries (`ACK_MAX_HOPS`).

### 6.2 ROUTE_REQUEST / RREQ (0x02)

Fixed size 30 bytes.

```
 +------------------------------- Header (12) -------------------------------+
 |                               Query ID                                    |
 +---------------------------------------------------------------------------+
 |                          Encrypted Source                                 |
 +---------------+---------------+-------------------------------------------+
 |   Hop Count   |    Metric     | Previous Hop                              |
 +---------------+---------------+-------------------------------------------+
 |                               RREQ Salt                                   |
 +---------------------------------------------------------------------------+
```

Destination-only source recovery uses static DH + time bucket + `rreq_salt`.

### 6.3 ROUTE_REPLY / RREP (0x03)

Fixed size 34 bytes.

```
 +------------------------------- Header (12) -------------------------------+
 |                               Query ID                                    |
 +---------------------------------------------------------------------------+
 |                            Replier Source                                 |
 +---------------------------------------------------------------------------+
 |                               Next Hop                                    |
 +---------------+---------------+-------------------------------------------+
 |   Hop Count   | Route Metric  | Auth HMAC (8 bytes)                      |
 +---------------+---------------+-------------------------------------------+
```

`Auth HMAC` is 8-byte truncated HMAC-SHA256 over bytes 0..25. Zero HMAC
indicates first-contact/unverified route bootstrap.

### 6.4 ROUTE_ERROR / RERR (0x04)

Fixed size 24 bytes.

```
 +------------------------------- Header (12) -------------------------------+
 |                            Reporter Address                               |
 +---------------------------------------------------------------------------+
 |                              Broken Dest                                  |
 +---------------------------------------------------------------------------+
 |                           Broken Next Hop                                 |
 +---------------------------------------------------------------------------+
```

### 6.5 BEACON (0x05)

Implementation carries fixed metadata plus optional name extension.

```
 +------------------------------- Header (12) -------------------------------+
 |                            Source Address                                 |
 +---------------------------------------------------------------------------+
 |                          PubKey Hash (4)                                  |
 +-----------------------+---------------+---------------+-------------------+
 | Uptime (min, 16 bits) | Battery %     | TX Queue      | Neighbor Count    |
 +-----------------------+---------------+---------------+-------------------+
 | Beacon Flags  |                 Network Time (32)                         |
 +---------------+-----------------------------------------------------------+
 | Time Confidence (16)   |             Auth HMAC (16 bytes)                 |
 +------------------------+--------------------------------------------------+
 | Name Len (opt) | Name bytes (opt, up to 16)                               |
 +------------------------------------------------------------------- -------+
```

`BEACON_FLAG_MAILBOX` indicates mailbox willingness.

### 6.6 KEY_EXCHANGE (0x06)

Fixed size 101 bytes.

```
 +------------------------------- Header (12) -------------------------------+
 |                            Source Address                                 |
 +---------------------------------------------------------------------------+
 |                    Ephemeral Public Key (32 bytes)                        |
 +--------------------------------------------------------------- ... -------+
 |                    Long-Term Public Key (32 bytes)                        |
 +--------------------------------------------------------------- ... -------+
 | Key ID | KE Type |                  Auth Tag (16 bytes)                   |
 +--------+---------+--------------------------------------------------------+
```

KE types: initiate/respond/confirm.

### 6.7 DELIVERY_RECEIPT (0x07)

Variable 22..54 bytes.

```
 +------------------------------- Header (12) -------------------------------+
 |                            Source Address                                 |
 +---------------------------------------------------------------------------+
 |                        Original Packet ID                                 |
 +---------------+---------------+-------------------------------------------+
 |   Hop Count   | Total Latency | Relay Path[0] (optional)                  |
 +---------------+---------------+-------------------------------------------+
 | Relay Path[1..N-1] (optional, max 8 hops total)                           |
 +--------------------------------------------------------------- ... -------+
```

### 6.8 CONGESTION (0x08)

Fixed size 20 bytes.

```
 +------------------------------- Header (12) -------------------------------+
 |                            Source Address                                 |
 +---------------------------------------------------------------------------+
 | Congestion Lvl | Queue Depth    | Est Clear Time (seconds)                |
 +----------------+----------------+-----------------------------------------+
```

### 6.9 TIME_SYNC (0x09)

Fixed size 24 bytes.

```
 +------------------------------- Header (12) -------------------------------+
 |                            Source Address                                 |
 +---------------------------------------------------------------------------+
 |                              Timestamp                                    |
 +---------------------------------------------------------------------------+
 | Confidence (ms)        | Stratum        | Sequence                         |
 +------------------------+----------------+----------------------------------+
```

### 6.10 DATA (0x0A)

Direct and channel messages both use DATA with encryption framing.

#### 6.10.1 Direct DATA

```
 +------------------------------- Header (12) -------------------------------+
 |                            Source Address                                 |
 +---------------------------------------------------------------------------+
 |                               Next Hop                                    |
 +---------------+---------------+-------------------------------------------+
 |   App Type    | Payload Len   | Nonce (12 bytes)                          |
 +---------------+---------------+-------------------------------------------+
 | Ciphertext (variable)                                                ...  |
 +---------------------------------------------------------------------------+
 |                         Auth Tag (16 bytes)                               |
 +---------------------------------------------------------------------------+
```

#### 6.10.2 Channel DATA

```
 +------------------------------- Header (12) -------------------------------+
 |                   Source Address field set to 0x00000000                  |
 +---------------------------------------------------------------------------+
 |                               Next Hop                                    |
 +---------------+-----------------------------------------------------------+
 | Payload Len   | Nonce (12 bytes)                                          |
 +---------------+-----------------------------------------------------------+
 | Ciphertext (contains channel_id, epoch, app_type, src_addr, payload) ... |
 +---------------------------------------------------------------------------+
 |                         Auth Tag (16 bytes)                               |
 +---------------------------------------------------------------------------+
```

Receivers MUST attempt decryption against configured channel keys.

### 6.11 STORE_REQUEST (0x0B)

```
 +------------------------------- Header (12) -------------------------------+
 |                              Source Addr                                  |
 +---------------------------------------------------------------------------+
 |                           Original Dest Addr                              |
 +---------------------------------------------------------------------------+
 |                           Original Packet ID                              |
 +---------------------------------------------------------------------------+
 |                            TTL Seconds                                    |
 +---------------------------------------------------------------------------+
 | Payload Len (16)         | Enclosed Payload ...                           |
 +--------------------------+-----------------------------------------------+
 | Auth HMAC (4)                                                            |
 +---------------------------------------------------------------------------+
```

### 6.12 STORE_ACK (0x0C)

```
 +------------------------------- Header (12) -------------------------------+
 |                             Mailbox Addr                                  |
 +---------------------------------------------------------------------------+
 |                          Original Packet ID                               |
 +---------------------------------------------------------------------------+
 | Status |                       Expires At                                 |
 +--------+------------------------------------------------------------------+
```

### 6.13 MAILBOX_DELIVERY (0x0D)

```
 +------------------------------- Header (12) -------------------------------+
 |                             Mailbox Addr                                  |
 +---------------------------------------------------------------------------+
 |                           Original Source Addr                            |
 +---------------------------------------------------------------------------+
 |                           Original Packet ID                              |
 +---------------------------------------------------------------------------+
 |                              Stored At                                    |
 +---------------------------------------------------------------------------+
 | Payload Len (16)         | Enclosed Payload ...                           |
 +--------------------------+-----------------------------------------------+
```

### 6.14 MAILBOX_QUERY (0x0E)

```
 +------------------------------- Header (12) -------------------------------+
 |                              Source Addr                                  |
 +---------------------------------------------------------------------------+
 |                             Mailbox Addr                                  |
 +---------------------------------------------------------------------------+
 |                             Auth HMAC (4)                                 |
 +---------------------------------------------------------------------------+
```

### 6.15 EMERGENCY (0x0F)

```
 +------------------------------- Header (12) -------------------------------+
 |                              Source Addr                                  |
 +---------------------------------------------------------------------------+
 |                              Latitude E7                                  |
 +---------------------------------------------------------------------------+
 |                             Longitude E7                                  |
 +---------------------------------------------------------------------------+
 | Altitude (16)          | Battery %       | Timestamp                      |
 +------------------------+-----------------+--------------------------------+
 | Msg Len | Short Message (0..32 bytes)                                     |
 +---------+--------------------------------------------------------- --------+
```

Emergency packets use emergency relay semantics (`HEADER_FLAG_EMERGENCY`).

### 6.16 EMERGENCY_CANCEL (0x10)

```
 +------------------------------- Header (12) -------------------------------+
 |                              Source Addr                                  |
 +---------------------------------------------------------------------------+
 |                          Cancel Timestamp                                 |
 +---------------------------------------------------------------------------+
 |                             Auth Tag (4)                                  |
 +---------------------------------------------------------------------------+
```

### 6.17 CODED (0x11)

```
 +------------------------------- Header (12) -------------------------------+
 | Num Components | Component ID 1                                           |
 +----------------+----------------------------------------------------------+
 | Component Len1 (16) | Component ID 2                                      |
 +---------------------+-----------------------------------------------------+
 | Component Len2 (16) | Coded Payload ...                                   |
 +---------------------+-----------------------------------------------------+
```

Current design uses two-component XOR coding.

### 6.18 PKT_TYPE_PROBE (0x12)

Probe/ACK formats are compact 12-byte-header-like structures.

```
 +---------------+---------------+---------------+---------------+
 |    Version    |      Type     | Probe Flags   |   Hop Limit   |
 +---------------+---------------+---------------+---------------+
 |                            Source Addr                         |
 +---------------------------------------------------------------+
 |                             Probe ID                           |
 +---------------------------------------------------------------+
```

### 6.19 PKT_TYPE_PROBE_ACK (0x13)

```
 +---------------+---------------+---------------+---------------+
 |    Version    |      Type     | Ack Flags     |   Hop Count   |
 +---------------+---------------+---------------+---------------+
 |                            Source Addr                         |
 +---------------------------------------------------------------+
 |                             Probe ID                           |
 +---------------------------------------------------------------+
 | RSSI (opt) | Path[0..N] (optional)                             |
 +------------+-------------------------------------------- ------+
```

### 6.20 LOCATION (0x14)

Location is a dedicated packet class in implementation registry. Payload modes:

* FULL: 17 bytes (`latitude_e7`, `longitude_e7`, `altitude_m`, `accuracy`,
  `speed`, `heading/2`, `timestamp`)
* COARSE: 5 bytes (`grid_lat`, `grid_lon`, `ts_low`)
* PRESENCE: 1 byte status

The LOCATION packet uses common header semantics and tier bits for reliability.

## 7. Fragmentation and Reassembly

### 7.1 Fragment Header

When `F1:F0` indicates fragmented payload, a 4-byte fragment header MUST be
present before fragment payload:

```
 +---------------+---------------+-------------------------------+
 | Frag Index    | Frag Total    | Message ID (16)               |
 +---------------+---------------+-------------------------------+
```

`FRAG_HEADER_SIZE = 4`.

### 7.2 Ordering and Security

Bramble fragments plaintext before encryption. Each fragment is encrypted and
authenticated independently (distinct nonce + 16-byte GCM tag).

This means:

* Per-fragment auth overhead increases;
* Forged fragments can be dropped immediately;
* Fragment buffers remain bounded.

### 7.3 Limits and Reassembly Rules

* Maximum fragments per message: 4
* Max per-fragment plaintext: 154 bytes
* Max reassembled plaintext: 616 bytes
* Reassembly timeout: 30 seconds
* Duplicate fragment: drop silently after auth verification
* Missing fragment at timeout: discard reassembly context

Implementations MUST cap concurrent reassemblies to configured limits (spec
example: 4 concurrent contexts).

## 8. Identity and Key Management

### 8.1 Node Identity

On first boot, nodes generate X25519 long-term keys and derive `node_addr` as
the first 4 bytes of SHA-256(public_key). Keys MUST be stored in NVS.

### 8.2 Direct Message Key Exchange

DM session keys are derived via X25519 + HKDF (`bramble-dm-v1`) using static
and ephemeral contributions. Response and confirm steps include truncated HMAC
validation.

DM key rotation SHOULD occur:

* Time-based: every 24 hours;
* Message-count-based: before nonce-space risk (`2^16` messages);
* Manual rotation MAY be user-triggered.

### 8.3 Channel Keys and Epoch Ratchet

Channel keys are pre-shared out-of-band and derived with HKDF
(`bramble-channel-v1`). Up to 16 channels are supported.

Epoch advancement uses HKDF (`bramble-channel-epoch`) and SHOULD trigger every
24h or every 256 messages, whichever occurs first.

Old epoch keys MUST be deleted after advancement (backward secrecy).

### 8.4 Public Channel

Channel index 0 is reserved for a well-known public PSK context
("Bramble Common"). It is intentionally non-confidential. Implementations MUST
apply stricter rate limiting on this channel.

### 8.5 RREQ Source Privacy Modes

Default mode encrypts RREQ source field for destination-only decryption. If
destination public key is unknown and policy allows, OPEN_SOURCE fallback MAY
transmit plaintext source for first-contact bootstrap.

## 9. Routing Protocol

### 9.1 Model

Bramble uses reactive route discovery inspired by AODV with route caches,
reverse-route tracking for RREP return, and route aging states.

### 9.2 Route Discovery Lifecycle

When no active route exists:

1. Queue outbound packet(s);
2. Emit RREQ (attempt 1 immediately);
3. Retry at +5s (wider scope), then +15s (max scope);
4. Fail discovery after third attempt.

Intermediate nodes MUST deduplicate RREQs and decrement hop limit. Destination
MUST generate RREP and include authentication HMAC when shared secret exists.

### 9.3 Route States

States: DISCOVERING, UNVERIFIED, ACTIVE, STALE, BROKEN, DELETED.

* `ACTIVE -> STALE` after `ROUTE_ACTIVE_TIMEOUT = 300s` of non-use;
* `STALE -> DELETED` after `ROUTE_STALE_TIMEOUT = 600s`;
* Hard eviction at `ROUTE_HARD_TIMEOUT = 3600s`.

### 9.4 Failure Handling

Forwarding failures increment per-route fail counters. After threshold (spec:
3), route is marked BROKEN, RERR is propagated, and alternate route/local
repair is attempted if available.

### 9.5 Channel Routing Behavior

Channel traffic is constrained flooding (bounded hop limit, dedup, jittered
rebroadcast). Relays can forward channel traffic without knowing channel ID
because channel metadata is encrypted inside payload.

### 9.6 Adaptive Metrics

A composite metric (0..255) weights link quality, delivery success, airtime,
and latency using integer-weighted factors (102, 77, 51, 26).

## 10. Reliability Model

### 10.1 Tiers

| Tier | Flags | ACK/Receipt | Retries |
|---|---|---|---|
| Broadcast | `TIER=00` | none | 0 |
| Normal | `TIER=01, ACK_REQ=1` | end-to-end ACK | 3 |
| Critical | `TIER=10, ACK_REQ=1, RECEIPT=1` | ACK + delivery receipt path | 8 |

### 10.2 Retry Backoff

Normal base delay is 2s; critical base delay is 3s; exponential backoff with
jitter is REQUIRED for retransmission scheduling.

### 10.3 Pending ACK Table

Pending reliability state MUST be bounded (spec reference: 8 concurrent entries
with cached packet bodies for retransmit).

### 10.4 Delivery Receipts and Path Telemetry

Critical-tier traffic MAY request relay path telemetry via DELIVERY_RECEIPT.
Relay nodes append addresses up to max hops (8). Implementations SHOULD use
this data for route quality updates.

### 10.5 Duplicate Detection

All packets MUST pass dedup checks keyed by packet ID with time-based eviction.
Probe ACK dedup SHOULD include responder/round semantics to avoid collapsing
valid multi-round observations.

## 11. Time Synchronization

### 11.1 Sources and Stratum

Time is carried in BEACON and TIME_SYNC packets.

Stratum conventions:

* 0: GPS synchronized
* 1..N: hop distance from better source
* 15: unsynchronized

### 11.2 Sync Acceptance Rules

Nodes SHOULD reject worse-stratum or worse-confidence updates unless unsynced.
Large offset changes MUST be clamped (spec example: ±5 seconds per sync).
Stratum-0 claims SHOULD require corroboration from multiple neighbors before
adoption.

### 11.3 Emission Policy

Only high-quality time sources (stratum <= 2) SHOULD proactively emit TIME_SYNC
periodically (spec example: 300s). Other nodes propagate time passively via
beacons (spec example: 60s intervals).

### 11.4 Replay Window

Timestamp checks SHOULD enforce approximately ±30-second acceptance window,
combined with packet-id dedup and nonce uniqueness tracking.

## 12. Resource Constraints and Limits

Implementations on ESP32-S3 MUST maintain hard limits to preserve stability.
Representative limits from protocol specification include:

* Routing table: 64
* Neighbor table: 32
* Pending discoveries: 8
* RREQ dedup: 128
* Reverse routes: 32
* Packet dedup: 128..256 (implementation-dependent)
* Pending ACKs: 8
* TX queue: 16
* Fragment contexts: 4
* Key cache: 32
* Peer pubkey cache: 64

The protocol RAM subtotal is ~33 KB in the planning model; system headroom is
maintained for RTOS, BLE, crypto working memory, and queue bursts.

## 13. Security Considerations

### 13.1 Threat Model

Bramble assumes local passive listeners, active packet injectors, and malicious
mesh participants. It does not claim protection against a global omnipresent
passive adversary.

### 13.2 Confidentiality and Integrity

* DM and channel payloads use AES-256-GCM;
* DM keys derive from X25519/HKDF exchanges;
* Channel keys derive from PSK + epoch ratchet;
* Encrypted packets MUST fail closed on auth-tag mismatch.

### 13.3 Routing and Control Authenticity

* RREP authentication uses truncated HMAC over route reply base fields;
* First-contact unauthenticated RREP is explicitly marked unverified and MUST
  be promoted only after key exchange success;
* Beacon trust is differentiated by known-peer key material.

### 13.4 Replay Protection

Replay defense combines packet ID dedup windows, nonce uniqueness discipline,
and timestamp validation windows.

### 13.5 Flood/DoS Mitigation

Mitigations include airtime budget, RREQ rate limiting, beacon throttling,
priority queueing, and duplicate suppression.

### 13.6 Sybil Resistance and Residual Risk

Sybil resistance is heuristic and partial (airtime economics, behavior-based
neighbor suspicion). It is NOT cryptographically strong identity admission.

### 13.7 Metadata Leakage

Even with encrypted payloads, observers can infer packet timing, size, and some
address/path metadata. Channel sender/channel ID concealment applies only to
non-members; members can identify senders by design.

### 13.8 Compromised Nodes

Compromise exposure is scoped by key domain:

* DM compromise affects pairs involving that node;
* Channel compromise affects channels where compromised node has key;
* Route sabotage is mitigated by retries and alternate route discovery.

## 14. IANA Considerations

IANA is requested to create a new registry:

**Registry Name:** Bramble Packet Types

**Registration Procedure:** Specification Required

Initial values are those in Section 5 (0x01..0x14). Values not listed are
unassigned. Future assignments SHOULD preserve wire compatibility with common
header semantics.

## 15. References

### 15.1 Normative References

* RFC 2119, Key words for use in RFCs to Indicate Requirement Levels.
* RFC 8174, Ambiguity of Uppercase vs Lowercase in RFC 2119 Key Words.

### 15.2 Informative References

* Bramble protocol specification: `docs/bramble-protocol-spec.md`
* Bramble packet constants: `components/packet/include/packet.h`
* Bramble mesh behavior: `main/mesh_task.c`

## 16. Implementation Snapshot Notes

This document reflects implementation-oriented protocol behavior and constants
from the Bramble firmware/spec corpus at the time of writing.

Key version anchors:

* Protocol/API version: `0.5.0`
* Wire version: `1`
* Packet type range currently defined: `0x01` through `0x14`

This draft is intended as an interoperability and engineering reference, not a
standards-track Internet standard.
