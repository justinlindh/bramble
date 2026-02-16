# Bramble vs Meshtastic vs MeshCore

A technical comparison of three LoRa mesh networking approaches.

---

## Overview

### Meshtastic

Meshtastic is the dominant open-source LoRa mesh networking project. Started around 2020, it has a large community (100k+ Discord members), broad hardware support (ESP32, nRF52, RP2040, Linux), and mature companion apps on Android, iOS, and web. Currently at v2.5.x stable with v2.6 in preview, it's the most battle-tested option and the default choice for consumer LoRa mesh. The project is community-driven with significant contributions from many developers.

**Maturity:** Production-ready. Thousands of nodes deployed worldwide. Well-documented.

### MeshCore

MeshCore is a newer open-source project (emerged ~early 2025) created by "ripplebiz" (Andy Kirby) and community contributors. It's a lightweight C++ library focused on multi-hop packet routing for LoRa radios. MeshCore positions itself between Meshtastic's consumer focus and Reticulum's advanced networking — aiming for simplicity with better scalability than pure flooding. It has companion apps on Android, iOS, and web, a web flasher, and growing community adoption (active Discord).

**Maturity:** Early but functional. Pre-1.0, rapidly evolving. No formal protocol spec — the code *is* the spec. V2 protocol spec is on the roadmap.

### Bramble

Bramble is a from-scratch LoRa mesh protocol design (currently at v0.1-draft, design document phase — no implementation yet). It targets ESP32 + SX1262/SX1276 hardware and prioritizes privacy-first design, reactive routing, tiered reliability, and airtime management. Designed for 50–200+ node meshes.

**Maturity:** Design document only. No code, no users, no ecosystem. Vaporware until implemented.

---

## Architecture Comparison

| Feature | Meshtastic | MeshCore | Bramble |
|---|---|---|---|
| **Routing approach** | Managed flooding (all nodes rebroadcast). DMs getting next-hop routing in v2.6. | Flooding with optimizations (dedup, hop limit, RSSI prioritization, random delay). Companion nodes don't repeat. | Reactive AODV-style routing with cached routes. Flooding only for channel/group messages (hop-limited). |
| **Encryption — channels** | AES256-CTR with PSK. No integrity check (no AEAD — issue #4030 open). Known-plaintext attacks possible. | AES-256 with PSK (details sparse; encrypt-then-MAC with shared secret per the codebase). | AES-256-GCM (AEAD) with PSK + epoch-based key ratchet for backward secrecy. Channel ID encrypted inside ciphertext. |
| **Encryption — DMs** | Since v2.5: X25519 PKC + AES-CCM. Prior: just channel PSK (no real DM privacy). | Ed25519 identity + ECDH key exchange → AES-128 encrypt-then-MAC. Signed adverts prevent spoofing. | X25519 Double-DH (ephemeral + static) → AES-256-GCM. 3-step key exchange with forward secrecy. |
| **Scalability target** | ~80–100 nodes practical ceiling (flooding). Degrades above ~30 active senders. | Similar to Meshtastic (flooding-based). Claims up to 64 hops theoretically; practical limits similar. | 50–200+ nodes. Route-based forwarding scales O(path_length) not O(N). |
| **Reliability model** | Optional ACKs. Basic retries. No flow control. No congestion awareness. | ACK mechanism. Basic retry logic. No formal congestion management. | Three tiers (Broadcast/Normal/Critical). Exponential backoff retries. Sliding window flow control. Congestion detection and response. |
| **Airtime management** | None built-in. Nodes transmit freely. Some duty cycle awareness in EU regions. | Flood advert interval configurable (default 12h for repeaters). Companion nodes don't repeat. | Token-bucket airtime budget (10% self-imposed duty cycle) with per-tier sub-budgets and priority queuing. |
| **Time sync** | GPS-based or NTP via WiFi/MQTT. No mesh-internal time sync protocol. | Not documented. Likely relies on GPS or companion device time. | Stratum-based mesh time sync via beacons + TIME_SYNC packets. GPS optional. ±1–2s convergence. |
| **Hardware targets** | ESP32, nRF52, RP2040, Linux. Dozens of boards. | ESP32, nRF52, various Heltec/LILYGO/RAK boards. Growing device list. | ESP32-S3 only (Heltec V3, T-Beam). Narrow focus by design. |
| **Protocol overhead** | Protobuf-encoded. Header ~16 bytes + protobuf payload. | 8-byte header + 2-byte CRC = 10 bytes overhead. Compact binary. | 12-byte header. Compact binary. No protobuf/JSON. |
| **Node identity** | Hardware MAC-based (4 bytes). Not cryptographically derived. Trivially spoofable on channels. | Ed25519 public key. Cryptographic identity. Signed adverts. | X25519 public key → SHA-256 → 4-byte address. Cryptographic identity. |
| **Max hops** | 7 (configurable) | 3–7 default, up to 64 theoretical | 8 (hard limit) |

---

## Routing

### Meshtastic: Managed Flooding

Meshtastic uses **managed flood routing** for all messages. Every node that receives a packet rebroadcasts it (up to the hop limit), with one key optimization: before rebroadcasting, a node listens briefly to see if another node has already rebroadcast the same packet. If so, it suppresses its own rebroadcast ("implicit ACK"). Nodes can be configured with roles (CLIENT, CLIENT_MUTE, ROUTER, ROUTER_LATE) to influence rebroadcast timing and priority.

**v2.6 addition:** Next-hop routing for DMs. After an initial flooded exchange, the node learns which neighbor successfully relayed the message, and subsequent DMs to that destination are sent directly to that next-hop. Falls back to flooding on the last retry attempt if next-hop fails. This is a significant improvement but only applies to unicast DMs, not channel messages.

**Scalability implications:** O(N) transmissions per message (every node rebroadcasts). With N nodes, a single message generates up to N transmissions. For channel messages, this is unavoidable in any system. For DMs, next-hop routing (v2.6) reduces this to O(path_length) after route learning. In practice, networks above ~80 nodes experience airtime congestion, especially with many active senders. The lack of airtime budgeting means one chatty node can monopolize the channel.

### MeshCore: Optimized Flooding

MeshCore also uses **flood routing** as its core mechanism, but with several optimizations:

- **Duplicate detection** via unique packet IDs and a recent-messages cache
- **Hop count limiting** (default 3, configurable up to 7, protocol supports 64)
- **Random backoff delays** before rebroadcast to reduce collisions
- **RSSI-based prioritization** — weaker signals forwarded with lower priority
- **Role-based forwarding** — "Companion" nodes don't repeat messages at all, reducing unnecessary rebroadcasts

MeshCore's approach is architecturally similar to Meshtastic's managed flooding. The key difference is the explicit role system: companion nodes (user handsets connected to phones) never repeat, and only dedicated repeater nodes forward traffic. This reduces the amplification factor in networks with many end-user devices.

**Scalability implications:** Still O(N_repeaters) per message. Better than Meshtastic in practice because only repeater nodes flood (companions don't repeat), but fundamentally limited by the same flooding architecture. No route caching or reactive discovery.

### Bramble: Reactive AODV-Style Routing

Bramble uses **on-demand (reactive) routing** for DMs. Routes are discovered only when needed via Route Request (RREQ) / Route Reply (RREP) packets, then cached and reused:

1. Source broadcasts RREQ (this *does* flood, similar to Meshtastic)
2. Destination unicasts RREP back along the reverse path
3. Subsequent data packets follow the discovered route — only nodes on the path transmit
4. Routes are cached with soft/hard timeouts and maintained via broken-link detection

**Channel messages** still use controlled flooding (hop-limited, max 3–6 hops), since group messages inherently need to reach all members.

**Privacy enhancement:** RREQ source address is encrypted — only the destination can determine who's looking for them. Intermediate nodes relay the RREQ without knowing the originator.

**Scalability implications:** Route discovery floods once. After that, DM traffic is O(path_length) — only 4–8 transmissions for an 8-hop path vs. potentially hundreds in a large flooded mesh. This is where Bramble's design most differs from both Meshtastic and MeshCore. The tradeoff is complexity: route maintenance, broken-link detection, and the RREQ/RREP exchange add protocol overhead and implementation complexity.

---

## Privacy & Encryption

### Meshtastic

| Aspect | Details |
|---|---|
| **Channel encryption** | AES256-CTR with PSK. **No integrity check** (CTR mode without MAC). Vulnerable to known-plaintext attacks — anyone who knows the PSK can forge messages impersonating any node. Issue #4030 tracks adding AEAD. |
| **DM encryption (v2.5+)** | X25519 key exchange + AES-CCM. PKC provides real E2E encryption. Messages signed with sender's private key for authentication. Major improvement over pre-2.5. |
| **DM encryption (pre-2.5)** | DMs were just channel messages with a destination field. Same PSK. No privacy from other channel members. |
| **Forward secrecy** | None for channels. DM key exchange provides some, but no session ratcheting or ephemeral key rotation. |
| **Metadata leakage** | Header is always sent unencrypted: source node ID, destination node ID, hop count, packet ID. All relay nodes (and passive observers) see who is talking to whom. |
| **Key management** | PSK shared out-of-band (QR code). No key rotation mechanism. Node identity based on MAC address (not cryptographic). |
| **Authentication** | Channel: none. Anyone with PSK can impersonate. DMs (v2.5+): PKC provides authentication. |

### MeshCore

| Aspect | Details |
|---|---|
| **Identity** | Ed25519 key pair per node. Identity is cryptographic. Adverts (name/position/public key broadcasts) are signed to prevent spoofing. |
| **DM encryption** | ECDH key exchange using Ed25519 keys → shared secret → AES-128 encrypt-then-MAC. Destination hash in packet header (not full address). |
| **Channel/room encryption** | AES-256 mentioned in docs. Room servers provide shared communication spaces. Details on key management are sparse. |
| **Forward secrecy** | Not documented. Likely no session ratcheting given the embedded focus. |
| **Metadata leakage** | Packet header includes source address (2 bytes) and destination address (2 bytes). Observable by all relay nodes and passive listeners. 16-bit addresses are short but still identifying. |
| **Key management** | Users manually broadcast "adverts" containing their name, position, and public key. No automatic key exchange — user-initiated. |

**Note:** MeshCore's crypto documentation is limited. The codebase uses the `orlp/ed25519` library for identity and ECDH, and custom encrypt-then-MAC for payloads. A V2 protocol spec is planned which may formalize encryption details.

### Bramble

| Aspect | Details |
|---|---|
| **Channel encryption** | AES-256-GCM (AEAD — provides both confidentiality and integrity). Channel ID is inside the ciphertext — non-members cannot determine which channel a message belongs to. Source address also encrypted inside ciphertext. Epoch-based key ratchet provides backward secrecy. |
| **DM encryption** | X25519 Double-DH (ephemeral × static + static × static) → HKDF-SHA256 → AES-256-GCM. 3-step key exchange (initiate → respond → confirm) with final key incorporating both parties' ephemeral keys for forward secrecy. |
| **Forward secrecy** | DMs: Yes, via ephemeral key pairs in key exchange. Channels: Backward secrecy via epoch ratchet (old keys deleted). No forward secrecy for channels (would require interactive group key exchange, impractical over LoRa). |
| **Metadata leakage** | Minimized by design. DM packets expose `dest_addr` and `next_hop` but NOT source. RREQ source is encrypted. Channel messages show `0x00000000` as source, and channel ID is inside ciphertext. Passive observer learns less than with Meshtastic or MeshCore. |
| **Key management** | Auto-generated X25519 identity on first boot. DM keys rotate every 24h or 65,536 messages. Channel keys rotate via epoch ratchet (every 24h or 256 messages). Offline nodes can catch up via HKDF chain. |
| **Authentication** | DMs: implicit via AES-GCM auth tag (only holder of session key can produce valid tag). Beacons: HMAC'd with pairwise session key for known peers. RREPs: HMAC'd with static DH shared secret. |

### Summary

Bramble's privacy design is significantly more ambitious than both Meshtastic and MeshCore. Key differentiators:

1. **Source privacy**: Bramble hides the message originator from relay nodes. Meshtastic and MeshCore expose source in the header.
2. **Channel privacy**: Bramble hides which channel a message belongs to. Meshtastic and MeshCore don't.
3. **Integrity**: Bramble uses AEAD (GCM) everywhere. Meshtastic channels still lack integrity checking. MeshCore uses encrypt-then-MAC.
4. **Key rotation**: Bramble has automatic rotation with backward secrecy. Meshtastic requires manual PSK changes. MeshCore undocumented.

---

## Reliability

### Meshtastic

- **ACKs:** Optional, per-message. Only for DMs (unicast). No ACK for channel messages.
- **Retries:** Basic retries (typically 3 attempts). Fixed timing, no exponential backoff.
- **Flow control:** None. No awareness of downstream congestion.
- **Delivery confirmation:** ACK tells sender the message arrived. No relay path information.
- **Congestion handling:** None built-in. The managed flooding suppression helps somewhat (nodes skip rebroadcast if they hear another node rebroadcasting), but there's no explicit congestion signaling or backpressure.

### MeshCore

- **ACKs:** Supported for unicast messages.
- **Retries:** Basic retry logic with collision-avoidance delays.
- **Flow control:** Not documented. Companion nodes not repeating acts as implicit flow control.
- **Delivery confirmation:** ACK mechanism present.
- **Congestion handling:** Flood advert intervals reduced to 12h to minimize repeater overhead. No explicit congestion protocol.

### Bramble

- **ACKs:** Three tiers: Broadcast (no ACK), Normal (end-to-end ACK), Critical (ACK + delivery receipt with full relay path).
- **Retries:** Exponential backoff. Normal: 3 retries over ~15s. Critical: 8 retries over ~12–16 minutes. Jitter to avoid synchronization.
- **Flow control:** Per-destination sliding window (max 4 unacknowledged packets). Additive increase, multiplicative decrease (AIMD). Prevents fast sender from overwhelming the mesh.
- **Delivery confirmation:** Critical tier includes delivery receipts with complete relay path — sender passively learns network topology.
- **Congestion handling:** Explicit CONGESTION packets broadcast by overwhelmed nodes. Four levels trigger progressive shedding (drop broadcast → drop normal → reduce TX power). Priority queue ensures critical traffic survives congestion.

---

## Scalability

### Meshtastic

**Realistic limit: ~80–100 nodes, ~30 active senders.**

Every message from every sender generates up to N rebroadcasts. With 100 nodes and 10 messages/minute, that's ~1,000 transmissions/minute. At ~500ms per transmission (SF10), that's 500 seconds of airtime per minute — clearly impossible on a single channel. In practice, with SF7 (faster) and moderate traffic, Meshtastic works well up to ~80 nodes. Beyond that, airtime saturation causes packet loss, delayed delivery, and beacon storms. The v2.6 next-hop routing for DMs significantly helps by reducing DM traffic to O(path_length), but channel messages still flood.

### MeshCore

**Realistic limit: Similar to Meshtastic, possibly slightly better.**

MeshCore's role system (companions don't repeat) means the effective flooding fan-out is limited to repeater nodes only. In a network with 100 devices where 80 are companions and 20 are repeaters, a message generates ~20 rebroadcasts instead of ~100. This is a meaningful practical improvement. However, the fundamental scaling is still O(N_repeaters) per message. MeshCore's 12-hour flood advert interval also helps reduce background noise.

### Bramble

**Target: 50–200+ nodes, 8-hop paths.**

DM traffic scales as O(path_length) after route discovery. A 5-hop DM generates 5 transmissions (plus the initial RREQ flood for route discovery, amortized across multiple messages). The token-bucket airtime budget (10% duty cycle, ~750 short packets/hour) prevents any single node from monopolizing the channel. Channel messages still flood (limited to 3–6 hops), but channels are explicitly a lower-tier service.

**The math:** At SF10/125kHz with 10% duty cycle, each node gets ~360s of airtime/hour. A 100-byte message takes ~480ms. That's ~750 messages/hour per node. With route-based forwarding, a 200-node mesh where each node sends 5 DMs/hour generates 5 × 5 (avg hops) × 200 = 5,000 relay transmissions/hour across the mesh, distributed among 200 nodes = 25 relay transmissions per node/hour. Well within budget. The same scenario with flooding would be 5 × 200 × 200 = 200,000 transmissions — impossible.

**Caveat:** These are design-stage projections. Real-world performance depends on topology, interference, hidden node problems, and implementation quality. Bramble has zero real-world data.

---

## Resource Usage

| Resource | Meshtastic | MeshCore | Bramble |
|---|---|---|---|
| **RAM** | ~100–200 KB (varies by platform and features enabled — BLE, WiFi, GPS all add overhead) | Lightweight — "no dynamic allocation except during setup." Designed for minimal footprint. Exact figures not published. | ~127 KB total (20 KB protocol, 5 KB app, 18 KB RTOS, 84 KB system). 60% headroom on ESP32-S3 (320 KB available). |
| **Flash** | ~1.5–2 MB (ESP32 with all features) | Compact — prebuilt binaries available for many boards. Size not published. | ~1.75 MB projected (256 KB firmware, 512 KB ESP-IDF, OTA partitions). |
| **Battery life** | Good with sleep modes. nRF52 boards excel (~days to weeks). ESP32 boards: ~1–3 days typical with screen. | Claims low power. Companion nodes (no repeating) save significant energy. | Designed for ESP32 deep sleep (~10µA). Airtime budgeting inherently conserves battery. No real-world battery data. |
| **Platform breadth** | ESP32, nRF52, RP2040, Linux — very broad | ESP32, nRF52 — growing | ESP32-S3 only — narrow by design |

---

## Ecosystem & Maturity

| Aspect | Meshtastic | MeshCore | Bramble |
|---|---|---|---|
| **Community size** | Very large (100k+ Discord, active subreddit, many YouTube creators) | Growing (active Discord, community contributors, emerging content creators) | None (solo design project) |
| **Companion apps** | Android, iOS, Web, Python CLI, extensive third-party tools | Android, iOS, Web, NodeJS library, Python CLI | None (planned: BLE + web serial) |
| **Hardware support** | Dozens of boards across multiple architectures | ~15+ boards, growing. Web flasher available. | 2 boards (Heltec V3, T-Beam) |
| **Documentation** | Excellent. Official docs, community guides, YouTube tutorials. | Growing. FAQ, community site (meshcore.co.uk, localmesh.nl). No formal protocol spec. | One design document. |
| **Production readiness** | Yes. Deployed in real emergencies, events, and daily use worldwide. | Early adopter stage. Functional for basic use. Evolving rapidly. | No. Design only. |
| **Protocol spec** | Documented (mesh-algo page, protobuf definitions) | No formal spec. V2 spec on roadmap. Code is the reference. | Detailed design doc with packet formats, algorithms, pseudocode. |
| **MQTT/Internet bridge** | Yes, built-in. MQTT integration for internet bridging. | Not documented as a core feature. | Not planned initially. |
| **Web flasher** | Yes (flasher.meshtastic.org) | Yes (flasher.meshcore.co.uk) | Planned (Phase 6 of roadmap) |

---

## Why Bramble?

### Where Bramble Aims to Improve

1. **Routing scalability.** The fundamental architectural difference. Reactive routing means DM traffic doesn't flood the mesh. This is the single biggest design win — it's the difference between O(N) and O(path_length) transmissions per message. Neither Meshtastic (even with v2.6 next-hop) nor MeshCore have route caching with on-demand discovery.

2. **Privacy by design.** Bramble's encrypted RREQ sources, encrypted channel IDs, and hidden sender addresses on channel messages represent a meaningfully different privacy posture. Meshtastic leaks source/destination in plaintext headers. MeshCore leaks source/destination in headers. Bramble was designed from day one to minimize metadata exposure.

3. **Airtime economics.** The token-bucket airtime budget with per-tier sub-budgets is unique among the three. Neither Meshtastic nor MeshCore has any formal airtime management. In a large mesh, this is the difference between graceful degradation and chaotic congestion collapse.

4. **Reliability tiers.** Three tiers with different retry strategies, plus sliding-window flow control and explicit congestion signaling, give applications fine-grained control. Meshtastic's "optional ACK with fixed retries" is a blunt instrument by comparison.

5. **AEAD everywhere.** AES-256-GCM provides both confidentiality and integrity in a single pass. Meshtastic's channel encryption (AES256-CTR without MAC) is notably weaker — it lacks integrity checking entirely.

### Where Bramble Is at a Disadvantage

1. **It doesn't exist.** This cannot be overstated. Meshtastic has thousands of deployed nodes, years of real-world testing, and a thriving community. MeshCore is shipping firmware and has users. Bramble is a design document. The gap between design and working implementation is enormous, and many elegant designs fail on contact with reality.

2. **Reactive routing complexity.** AODV-style routing is well-understood in theory but tricky in practice over lossy LoRa links. Route discovery adds latency to the first message. Route maintenance (broken links, stale routes) adds implementation complexity. Meshtastic's managed flooding "just works" — it's stupid but robust.

3. **Hardware breadth.** ESP32-S3 only. No nRF52 (which has the best battery life in the LoRa ecosystem). No Linux. Meshtastic runs on everything.

4. **No ecosystem.** No apps. No community. No MQTT bridge. No integration with anything. Building an ecosystem from zero is arguably harder than building the protocol itself.

5. **Single developer.** Both Meshtastic and MeshCore benefit from community contributions and diverse perspectives. A solo project carries bus-factor risk and limited testing capacity.

6. **Channel messages still flood.** Bramble's routing improvements only help DMs. Channel/group messages still use controlled flooding (albeit hop-limited). For networks where most traffic is channel-based (many Meshtastic deployments), the routing advantage narrows.

7. **First-message latency.** Route discovery takes time — potentially 5–15 seconds for the RREQ/RREP round trip before the first DM can be sent. Meshtastic's flooding delivers (or drops) immediately. For time-sensitive first contact, this matters.

### Honest Assessment

Bramble's design addresses real, well-understood limitations of flooding-based mesh protocols. The privacy model is genuinely novel for the LoRa mesh space. The airtime budget system is sound engineering. If implemented correctly, it would handle larger meshes more gracefully than either Meshtastic or MeshCore.

But "if implemented correctly" is doing a lot of heavy lifting. Meshtastic's managed flooding has survived contact with reality in ways that a design document hasn't been tested against. MeshCore's simplicity is a feature, not a bug — simple systems fail in predictable ways.

The most likely path to value: implement Bramble's core ideas (reactive routing, privacy-preserving RREQ, airtime budgets) and prove they work in a 20-node testbed. That empirical validation is worth more than any amount of design documentation.
