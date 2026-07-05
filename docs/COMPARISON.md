# Bramble vs Meshtastic vs MeshCore

A technical comparison of three LoRa mesh networking approaches.

---

## Overview

### Meshtastic

Meshtastic is the dominant open-source LoRa mesh networking project. Started around 2020, it has a large community (100k+ Discord members), broad hardware support (ESP32, nRF52, RP2040, Linux), and mature companion apps on Android, iOS, and web. Currently at v2.5.x stable with v2.6 in preview, it's the most battle-tested option and the default choice for consumer LoRa mesh. The project is community-driven with significant contributions from many developers.

**Maturity:** Production-ready. Thousands of nodes deployed worldwide. Well-documented.

### MeshCore

MeshCore is a newer open-source project (emerged ~early 2025) created by "ripplebiz" (Andy Kirby) and community contributors. It's a lightweight C++ library focused on multi-hop packet routing for LoRa radios. MeshCore positions itself between Meshtastic's consumer focus and Reticulum's advanced networking: aiming for simplicity with better scalability than pure flooding. It has companion apps on Android, iOS, and web, a web flasher, and growing community adoption (active Discord).

**Maturity:** Early but functional. Pre-1.0, rapidly evolving. No formal protocol spec: the code *is* the spec. V2 protocol spec is on the roadmap.

### Bramble

Bramble is a from-scratch LoRa mesh protocol and firmware. It targets ESP32-S3 hardware (Heltec V3, T-Deck Plus, Heltec V4) and prioritizes privacy-first design, dual-substrate routing (reactive AODV by default with an opt-in flood transport), authenticated traffic, confirmable delivery, end-to-end DMs, tiered reliability, and airtime management. The project includes firmware, a Go SDK (`bramble-go`), a CLI (`bramble-cli`), and a web companion app.

**Maturity:** Pre-alpha, solo developer project. Running on 3 boards, with working mesh, RPC, BLE, Wi-Fi, GPS, location sharing, mailbox store-and-forward, and a web companion app. No field deployments and no user base yet.

---

## Architecture Comparison

| Feature | Meshtastic | MeshCore | Bramble |
|---|---|---|---|
| **Routing approach** | Managed flooding (all nodes rebroadcast). DMs getting next-hop routing in v2.6. | Flood-based route discovery, then data follows the learned direct path. Companion nodes don't repeat. | Dual-substrate. Reactive AODV (default): RREQ/RREP discovery, cached routes, reverse-route breadcrumbs from DATA, intermediate-node RREP, route-forwarded ACKs. Opt-in flooding transport (`s_flood_transport`, default off): hop-limited, deduplicated, airtime-budget-gated floods with a flooded ACK. Channel/group messages relay multi-hop via the flood relay. |
| **Encryption: channels** | AES256-CTR with PSK. No integrity check (no AEAD: issue #4030 open). Known-plaintext attacks possible. | AES-256 with PSK (details sparse; encrypt-then-MAC with shared secret per the codebase). | AES-256-GCM (AEAD) with PSK. Channel ID encrypted inside ciphertext. |
| **Encryption: DMs** | Since v2.5: X25519 PKC + AES-CCM. Prior: just channel PSK (no real DM privacy). | Ed25519 identity + ECDH key exchange → AES-128 encrypt-then-MAC. Signed adverts prevent spoofing. | Per-peer end-to-end AES-256-GCM sessions, keyed by a role-symmetric quad-DH X25519 exchange with a 7-digit SAS (`components/dm_session`). Not readable by other channel members. No forward secrecy; SAS-comparison UX not yet shipped (see [SECURITY-MODEL.md](SECURITY-MODEL.md)). |
| **Traffic authentication** | Channel traffic is unauthenticated: any PSK holder can forge/impersonate. DMs (v2.5+) are authenticated. | Per-node Ed25519 signatures; signed adverts. | DATA frames and the control plane (RREP, RERR, ACK, delivery receipt, beacon) carry a network-key HMAC that relays verify before acting. Residual: any network-key holder is an insider and can forge these MACs; an unprovisioned network falls back to a public PSK ([SECURITY-MODEL.md](SECURITY-MODEL.md)). |
| **Scalability target** | ~80–100 nodes practical ceiling (flooding). Degrades above ~30 active senders. | Flood-based discovery with direct-path data delivery. Claims up to 64 hops theoretically. | Reactive DM forwarding is O(path_length), not O(N). Honest measured scale under the collision-modeled simulator (the source of truth): ~95% delivery at 10 nodes, collapsing to ~10-12% at 50-100 and 0% at 200 as a single SF10 channel saturates under control-plane load ([results/simulation-2026-07-honest-baseline.md](results/simulation-2026-07-honest-baseline.md)). The earlier collision-free "100% at 200 nodes" figures were retracted as sim artifacts. No field test has been run. |
| **Reliability model** | Optional ACKs. Basic retries. No flow control. No congestion awareness. | ACK mechanism. Basic retry logic. No formal congestion management. | Three tiers (Broadcast/Normal/Critical). Exponential backoff retries with jitter, end-to-end ACKs, delivery receipts with relay path. Flow-control and congestion-signaling designs were removed unshipped; sender pacing comes from the per-tier airtime budget at the TX chokepoint and the bounded pending-ACK table. |
| **Airtime management** | None built-in. Nodes transmit freely. Some duty cycle awareness in EU regions. | Flood advert interval configurable (default 12h for repeaters). Companion nodes don't repeat. | Enforced: every transmission passes one budget-gated TX path with real time-on-air costing, per-tier token buckets, and regional duty-cycle caps where the frequency plan requires them (EU868 1%). No transmit path bypasses the budget. |
| **Time sync** | GPS-based or NTP via WiFi/MQTT. No mesh-internal time sync protocol. | Not documented. Likely relies on GPS or companion device time. | Stratum-based mesh time sync via beacon fields (corroboration-gated). GPS optional. |
| **Hardware targets** | ESP32, nRF52, RP2040, Linux. Dozens of boards. | ESP32, nRF52, various Heltec/LILYGO/RAK boards. Growing device list. | ESP32-S3 only (Heltec V3, T-Deck Plus, Heltec V4). Narrow focus by design. |
| **Protocol overhead** | Protobuf-encoded. Header ~16 bytes + protobuf payload. | 8-byte header + 2-byte CRC = 10 bytes overhead. Compact binary. | 12-byte base header, compact binary (no protobuf/JSON). Authenticated DATA adds a 28-byte envelope prefix (src_addr, relay-mutable prev_hop, 8-byte auth HMAC) plus a 12-byte nonce and 16-byte GCM tag. |
| **Node identity** | Hardware MAC-based (4 bytes). Not cryptographically derived. Trivially spoofable on channels. | Ed25519 public key. Cryptographic identity. Signed adverts. | X25519 public key → SHA-256 → 4-byte address. Cryptographic identity, but no per-node signature or trust anchor, so cross-fleet Sybil/impersonation is not closed. |
| **Max hops** | 7 (configurable) | 3–7 default, up to 64 theoretical | Reactive: expanding-ring discovery 4 then 8, max route depth 8. Flood transport: configurable 1..32, default 8. |

---

## Routing

### Meshtastic: Managed Flooding

Meshtastic uses **managed flood routing** for all messages. Every node that receives a packet rebroadcasts it (up to the hop limit), with one key optimization: before rebroadcasting, a node listens briefly to see if another node has already rebroadcast the same packet. If so, it suppresses its own rebroadcast ("implicit ACK"). Nodes can be configured with roles (CLIENT, CLIENT_MUTE, ROUTER, ROUTER_LATE) to influence rebroadcast timing and priority.

**v2.6 addition:** Next-hop routing for DMs. After an initial flooded exchange, the node learns which neighbor successfully relayed the message, and subsequent DMs to that destination are sent directly to that next-hop. Falls back to flooding on the last retry attempt if next-hop fails. This is a significant improvement but only applies to unicast DMs, not channel messages.

**Scalability implications:** O(N) transmissions per message (every node rebroadcasts). With N nodes, a single message generates up to N transmissions. For channel messages, this is unavoidable in any system. For DMs, next-hop routing (v2.6) reduces this to O(path_length) after route learning. In practice, networks above ~80 nodes experience airtime congestion, especially with many active senders. The lack of airtime budgeting means one chatty node can monopolize the channel.

### MeshCore: Flood Discovery, Then Direct Paths

MeshCore uses **flooding for route discovery**, after which data for known destinations follows the learned direct path. The flooding side carries several optimizations:

- **Duplicate detection** via unique packet IDs and a recent-messages cache
- **Hop count limiting** (default 3, configurable up to 7, protocol supports 64)
- **Random backoff delays** before rebroadcast to reduce collisions
- **RSSI-based prioritization**: weaker signals forwarded with lower priority
- **Role-based forwarding**: "Companion" nodes don't repeat messages at all, reducing unnecessary rebroadcasts

A key difference from Meshtastic is the explicit role system: companion nodes (user handsets connected to phones) never repeat, and only dedicated repeater nodes forward traffic. This reduces the amplification factor in networks with many end-user devices.

**Scalability implications:** Discovery floods are O(N_repeaters), and only repeater nodes rebroadcast (companions don't repeat). Data to known destinations follows the learned path rather than re-flooding.

### Bramble: Reactive AODV-Style Routing

Bramble uses **on-demand (reactive) routing** for DMs. Routes are discovered only when needed via Route Request (RREQ) / Route Reply (RREP) packets, then cached and reused:

1. Source broadcasts RREQ (this *does* flood, similar to Meshtastic)
2. Destination unicasts RREP back along the reverse path
3. Subsequent data packets follow the discovered route: only nodes on the path transmit
4. Routes are cached with soft/hard timeouts and maintained via broken-link detection

**Channel/group messages** use controlled flooding (hop-limited, default 8, configurable 1..32) and now relay multi-hop rather than the earlier single-hop-only behavior, since group messages inherently need to reach all members.

**Opt-in flood transport.** A runtime toggle (`s_flood_transport`, default off) can also route unicast DMs by the same hop-limited, deduplicated, budget-gated flood instead of reactive discovery, with a flooded ACK for route-free sender confirmation. It trades airtime for reach and is best-effort within the hop budget. Both substrates ship; the toggle selects one; reactive stays the default and is not removed.

**Privacy enhancement:** RREQ packets carry a per-query pseudonym instead of the originator's address, so forwarded route requests do not identify who is asking. The destination address stays cleartext, and the first hop can still infer the originator (a fresh RREQ has hop_count 0); see [SECURITY-MODEL.md](SECURITY-MODEL.md) for the precise scope.

**Scalability implications:** Route discovery floods once. After that, DM traffic is O(path_length): one transmission per hop (discovery uses an expanding ring: hop limit 4 on the first attempt, 8 on retries, and 8 is the protocol's maximum route depth) vs. potentially hundreds in a large flooded mesh. This is where Bramble's design most differs from both Meshtastic and MeshCore. The tradeoff is complexity: route maintenance, broken-link detection, and the RREQ/RREP exchange add protocol overhead and implementation complexity.

---

## Privacy & Encryption

### Meshtastic

| Aspect | Details |
|---|---|
| **Channel encryption** | AES256-CTR with PSK. **No integrity check** (CTR mode without MAC). Vulnerable to known-plaintext attacks: anyone who knows the PSK can forge messages impersonating any node. Issue #4030 tracks adding AEAD. |
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
| **Key management** | Users manually broadcast "adverts" containing their name, position, and public key. No automatic key exchange: user-initiated. |

**Note:** MeshCore's crypto documentation is limited. The codebase uses the `orlp/ed25519` library for identity and ECDH, and custom encrypt-then-MAC for payloads. A V2 protocol spec is planned which may formalize encryption details.

### Bramble

| Aspect | Details |
|---|---|
| **Channel encryption** | AES-256-GCM (AEAD: provides both confidentiality and integrity). Channel ID is inside the ciphertext, so non-members cannot determine which channel a message belongs to. Keys derive deterministically from the channel passphrase, so a passphrase holder can compute every epoch key. |
| **DM encryption** | Per-peer end-to-end AES-256-GCM sessions, keyed by a role-symmetric quad-DH X25519 exchange (four X25519 DHs mixed via HKDF-SHA256) with a 7-digit SAS for out-of-band verification (`components/dm_session`). The handshake travels inside DATA envelopes (`app_type = APP_TYPE_KE`); the standalone `KEY_EXCHANGE` packet type was retired from the wire. Other channel members cannot read a DM. |
| **Forward secrecy** | None. DM session keys are derived once per handshake and never ratcheted (rekey requires a new handshake). Channel epoch keys derive from the previous epoch's key, and everything derives from the passphrase, so a passphrase holder can compute every epoch. |
| **Metadata leakage** | The cleartext header carries the destination address, packet id, and flags, and DATA packets carry a cleartext 4-byte source address plus a relay-mutable prev_hop. RREQ sources are pseudonymized per query; channel ID is inside the ciphertext; LOCATION ciphertext is padded to a fixed size so it does not leak the sharing tier. |
| **Key management** | Auto-generated X25519 identity on first boot. No automatic key rotation; a channel epoch mechanism exists, and receivers catch up across epoch advances via rate-limited trial decryption. |
| **Authentication** | Channel/DM payloads: the AES-GCM auth tag (a channel tag is producible by any channel-key holder; a DM tag requires the per-peer session key). Control plane and DATA (RREP, RERR, ACK, delivery receipt, beacon, plus DATA's `auth_hmac`): network-key HMAC, verified by relays before acting. Residual: forgeable by any network-key insider, and by anyone under the public-PSK fallback before a network key is provisioned. RREQ itself carries no MAC (a broadcast route request has no pre-shared per-flow context). |

### Summary

Bramble's privacy and authentication posture is ambitious relative to Meshtastic; relative to MeshCore (which already has per-node keys and end-to-end DMs) the edge is narrower. Distinct points:

1. **Authenticated traffic**: DATA and the control plane carry a network-key HMAC that relays verify before acting, so an outsider on a provisioned network cannot forge or replay control/DATA frames. Meshtastic's channel traffic is unauthenticated (any PSK holder can impersonate). Residual: a network-key insider can still forge, and an unprovisioned network falls back to a public PSK (see [SECURITY-MODEL.md](SECURITY-MODEL.md)).
2. **Confirmed delivery**: acknowledged tiers report DELIVERED vs FAILED to the sender, a genuine edge over Meshtastic's fire-and-forget at low-to-moderate load. The flooded-confirmation half degrades under high load (airtime scales O(messages x nodes)), so it is not a high-load guarantee.
3. **End-to-end DMs**: per-peer quad-DH X25519 AES-256-GCM sessions, not channel-key DMs. This is parity with MeshCore and ahead of pre-2.5 Meshtastic.
4. **Metadata hygiene**: RREQ source pseudonymization and fixed-size, tiered LOCATION ciphertext. Integrity comes from AEAD (GCM); Meshtastic channels still lack integrity checking. Data packets still carry a cleartext source address.
5. **Channel privacy**: Bramble hides which channel a message belongs to. Meshtastic and MeshCore don't.

Versus MeshCore specifically the remaining edge is thin: confirmed broadcast delivery and authenticated-flood membership gating, weighed against MeshCore's more mature ecosystem and more efficient routing.

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
- **Retries:** Exponential backoff with ±25% jitter. Normal: 3 retries from a 2 s base. Critical: 8 retries from a 3 s base. A retry denied by the airtime budget burns the attempt, so a saturated mesh produces a visible FAILED status instead of a zombie pending message.
- **Flow control:** Designed and component-tested (per-destination sliding window with AIMD), but not yet wired into the live mesh path.
- **Delivery confirmation:** Critical tier includes delivery receipts with complete relay path: sender passively learns network topology.
- **Congestion handling:** The dedicated CONGESTION packet type is defined but not yet transmitted or handled. What ships today is indirect: per-tier airtime budgets shed lower-tier traffic first, and control traffic (routing, ACKs) has a reserved budget lane that data load cannot starve.

---

## Scalability

### Meshtastic

**Realistic limit: ~80–100 nodes, ~30 active senders.**

Every message from every sender generates up to N rebroadcasts. With 100 nodes and 10 messages/minute, that's ~1,000 transmissions/minute. At ~500ms per transmission (SF10), that's 500 seconds of airtime per minute: clearly impossible on a single channel. In practice, with SF7 (faster) and moderate traffic, Meshtastic works well up to ~80 nodes. Beyond that, airtime saturation causes packet loss, delayed delivery, and beacon storms. The v2.6 next-hop routing for DMs significantly helps by reducing DM traffic to O(path_length), but channel messages still flood.

### MeshCore

**Realistic limit: Similar to Meshtastic, possibly slightly better.**

MeshCore's role system (companions don't repeat) means the effective flooding fan-out is limited to repeater nodes only. In a network with 100 devices where 80 are companions and 20 are repeaters, a message generates ~20 rebroadcasts instead of ~100. This is a meaningful practical improvement. However, discovery traffic still scales O(N_repeaters); data to known destinations follows the learned direct path. MeshCore's 12-hour flood advert interval also helps reduce background noise.

### Bramble

Reactive DM traffic scales as O(path_length) after route discovery: one transmission per hop, plus the initial RREQ flood amortized across subsequent messages. Discovery uses an expanding ring (hop limit 4 first, 8 on retries); routes up to 8 hops deep are usable end-to-end. The token-bucket airtime budget caps each node's transmission time with per-tier sub-budgets, so no single node can monopolize the channel. Channel messages flood (hop-limited, multi-hop), and the opt-in flood transport (`s_flood_transport`, default off) can route DMs the same way when enabled, trading airtime for reach.

**Validation status:** scale behavior has not been validated in a real-world deployment; no multi-node field test has been run. The simulator runs the real protocol code over a collision-model radio layer (real time-on-air, collisions, capture, half-duplex, LBT) and is the source of truth. The honest July baseline ([results/simulation-2026-07-honest-baseline.md](results/simulation-2026-07-honest-baseline.md), which supersedes the June numbers for planning): about 95% message delivery at 10 nodes, collapsing to ~12% at 50, ~10% at 100, and 0% at 200. A single SF10 channel saturates under control-plane load (RREQ discovery storms amplifying a beacon floor) well before traffic flows, and the 200-node grid also spans 11 to 17 hops, beyond the 8-hop route ceiling. The earlier collision-free "100% at 200 nodes" numbers were retracted as sim artifacts. The design's scaling argument is a goal under active investigation, not a demonstrated property.

---

## Resource Usage

| Resource | Meshtastic | MeshCore | Bramble |
|---|---|---|---|
| **RAM** | ~100–200 KB (varies by platform and features enabled: BLE, WiFi, GPS all add overhead) | Lightweight: "no dynamic allocation except during setup." Designed for minimal footprint. Exact figures not published. | ~127 KB total (20 KB protocol, 5 KB app, 18 KB RTOS, 84 KB system). 60% headroom on ESP32-S3 (320 KB available). |
| **Flash** | ~1.5–2 MB (ESP32 with all features) | Compact: prebuilt binaries available for many boards. Size not published. | ~1.75 MB projected (256 KB firmware, 512 KB ESP-IDF, OTA partitions). |
| **Battery life** | Good with sleep modes. nRF52 boards excel (~days to weeks). ESP32 boards: ~1–3 days typical with screen. | Claims low power. Companion nodes (no repeating) save significant energy. | Designed for ESP32 deep sleep (~10µA). Airtime budgeting inherently conserves battery. No real-world battery data. |
| **Platform breadth** | ESP32, nRF52, RP2040, Linux: very broad | ESP32, nRF52: growing | ESP32-S3 only: narrow by design |

---

## Ecosystem & Maturity

| Aspect | Meshtastic | MeshCore | Bramble |
|---|---|---|---|
| **Community size** | Very large (100k+ Discord, active subreddit, many YouTube creators) | Growing (active Discord, community contributors, emerging content creators) | Solo developer project |
| **Companion apps** | Android, iOS, Web, Python CLI, extensive third-party tools | Android, iOS, Web, NodeJS library, Python CLI | Web companion app, Go SDK, CLI tool |
| **Hardware support** | Dozens of boards across multiple architectures | ~15+ boards, growing. Web flasher available. | 3 boards (Heltec V3, T-Deck Plus, Heltec V4) |
| **Documentation** | Excellent. Official docs, community guides, YouTube tutorials. | Growing. FAQ, community site (meshcore.co.uk, localmesh.nl). No formal protocol spec. | Protocol spec, architecture doc, code-verified security model, RPC reference, API docs |
| **Production readiness** | Yes. Deployed in real emergencies, events, and daily use worldwide. | Early adopter stage. Functional for basic use. Evolving rapidly. | Pre-production. Functional firmware on dev boards, not field-tested at scale. |
| **Protocol spec** | Documented (mesh-algo page, protobuf definitions) | No formal spec. V2 spec on roadmap. Code is the reference. | Detailed design doc with packet formats, algorithms, pseudocode. |
| **MQTT/Internet bridge** | Yes, built-in. MQTT integration for internet bridging. | Not documented as a core feature. | Not planned initially. |
| **Web flasher** | Yes (flasher.meshtastic.org) | Yes (flasher.meshcore.co.uk) | Yes (browser-based flasher in `web-flasher/`, hosted at bramblemesh.org) |

---

## Why Bramble?

### Where Bramble Aims to Improve

1. **Routing scalability, honestly bounded.** Reactive routing (the default) keeps DM traffic at O(path_length) instead of O(N), the core architectural bet. The payoff has not materialized at scale yet: a single SF10 channel still saturates on control traffic by ~50 nodes (see Scalability above), so this is a design argument under investigation, not a demonstrated win. An opt-in flood transport trades that argument for best-effort reach within a hop budget.

2. **Privacy by design.** Bramble's pseudonymized RREQ sources and encrypted channel IDs represent a meaningfully different privacy posture; [SECURITY-MODEL.md](SECURITY-MODEL.md) documents exactly what is and is not hidden today. Meshtastic and MeshCore carry source and destination in plaintext headers.

3. **Airtime economics.** The token-bucket airtime budget with per-tier sub-budgets, enforced at a single TX choke point with real time-on-air costing, is unique among the three. Neither Meshtastic nor MeshCore has any formal airtime management. In a large mesh, this is the difference between graceful degradation and chaotic congestion collapse.

4. **Reliability tiers.** Three tiers with different retry strategies, end-to-end ACKs, and path-tracing delivery receipts give applications fine-grained control (sliding-window flow control and congestion signaling were removed unshipped). Meshtastic's "optional ACK with fixed retries" is a blunt instrument by comparison.

5. **Authenticated traffic and AEAD payloads.** AES-256-GCM gives channel and DM payloads confidentiality and integrity in one pass (Meshtastic's AES256-CTR channels lack integrity entirely), and a network-key HMAC on DATA and the control plane lets relays reject forged or replayed frames from outsiders. The honest limit: a network-key insider can still forge, and an unprovisioned network uses a public-PSK fallback.

6. **Confirmable delivery.** Acknowledged tiers report DELIVERED vs FAILED rather than fire-and-forget. This is a real edge at low-to-moderate load; the flooded-confirmation half degrades under high message load (airtime scales O(messages x nodes)), so it is bounded, not a high-load guarantee.

7. **End-to-end DMs.** Per-peer quad-DH X25519 AES-256-GCM sessions, so channel members cannot read each other's DMs (parity with MeshCore, ahead of pre-2.5 Meshtastic).

### Where Bramble Is at a Disadvantage

1. **It's early.** Meshtastic has thousands of deployed nodes, years of real-world testing, and a thriving community. MeshCore is shipping firmware with a growing user base. Bramble has working firmware on 3 boards but no field deployments or community yet. The gap between a working prototype and a production mesh network is substantial.

2. **Reactive routing complexity.** AODV-style routing is well-understood in theory but tricky in practice over lossy LoRa links. Route discovery adds latency to the first message. Route maintenance (broken links, stale routes) adds implementation complexity. Meshtastic's managed flooding "just works": it's stupid but robust.

3. **Hardware breadth.** ESP32-S3 only (3 boards). No nRF52 (which has the best battery life in the LoRa ecosystem). No Linux. Meshtastic runs on everything.

4. **Small ecosystem.** Web app, Go SDK, and CLI exist, but no mobile apps, no MQTT bridge, no community contributors. Building an ecosystem from one developer is a significant challenge.

5. **Single developer.** Both Meshtastic and MeshCore benefit from community contributions and diverse perspectives. A solo project carries bus-factor risk and limited testing capacity.

6. **Channel traffic and flooded confirmation are airtime-bound.** Channel/group messages relay multi-hop by controlled flooding, so the reactive routing advantage applies mainly to DMs. The flood-based confirmed-delivery mode's airtime scales as O(messages x nodes): it holds confirmation only at low-to-moderate load and collapses under sustained high load. That load ceiling is close to fundamental to flooding, not a tuning bug.

7. **First-message latency.** Route discovery takes time: potentially 5–15 seconds for the RREQ/RREP round trip before the first DM can be sent. Meshtastic's flooding delivers (or drops) immediately. For time-sensitive first contact, this matters.

### Honest Assessment

Bramble's design addresses real, well-understood limitations of flooding-based mesh protocols. The privacy model is genuinely novel for the LoRa mesh space. The airtime budget system is sound engineering. If implemented correctly, it would handle larger meshes more gracefully than either Meshtastic or MeshCore.

But "if implemented correctly" is doing a lot of heavy lifting. Meshtastic's managed flooding has survived years of contact with reality that Bramble's running-but-young firmware has not. MeshCore's simplicity is a feature, not a bug: simple systems fail in predictable ways.

The most likely path to value: grow the testbed beyond 3 boards and prove the reactive routing, privacy-preserving RREQ, and airtime budgets work at scale in a 20+ node deployment. That empirical validation will close the gap between working firmware and a credible alternative.
