# Bramble vs Meshtastic vs MeshCore

A technical comparison of three LoRa mesh networking approaches.

> **Last verified against upstream sources: 2026-07-08** (Meshtastic firmware
> 2.7.26 Beta, 2026-06-24; MeshCore firmware v1.16.0, 2026-06-06). Comparison
> docs rot fast; all three projects ship regularly. Re-verify before
> relying on the claims here. Key sources are listed at the bottom.

---

## Overview

### Meshtastic

Meshtastic is the dominant open-source LoRa mesh networking project. Started around 2020, it has a large community (~51k Discord members as of 2026-07), broad hardware support (ESP32, nRF52, RP2040/RP2350, STM32WL, Linux), and mature companion apps on Android, iOS, and web. The 2.7 series is its recommended channel (2.7.26 "Beta", June 2026). Community-driven with institutional backing since 2025: Meshtastic Solutions Inc. supports development commercially while Meshtastic LLC holds the trademark.

**Maturity:** Production-ready. Thousands of nodes deployed worldwide; the largest known network was 2,000+ nodes at DEF CON 33 (Aug 2025), congested but functioning. Well-documented.

### MeshCore

MeshCore was started in late 2024 by Scott Powell ("ripplebiz", Ripple Radios, Australia) and launched publicly in early 2025 with Liam Cottle (companion apps) and Andy Kirby (promotion). It is a lightweight C++ library and firmware for multi-hop LoRa packet routing, positioned between Meshtastic's consumer focus and Reticulum's advanced networking: simplicity with better scalability than pure flooding.

**Governance fork (April 2026):** a governance split moved the official open project to `github.com/meshcore-dev/MeshCore` with sites at meshcore.io, docs.meshcore.io, and flasher.meshcore.io (Discord: meshcore.gg). meshcore.co.uk hosts **MeshOS**, an unaffiliated proprietary per-device-licensed fork, and does not represent the open project.

**Maturity:** Shipping and past the early-adopter stage for basic messaging: v1.16.0 with a regular release cadence, 50+ supported devices, an official protocol reference at docs.meshcore.io, large regional meshes (UK, NL, US metros, Australia), and vendors shipping MeshCore-preloaded hardware. Core firmware is MIT; the primary companion apps are closed-source freemium.

### Bramble

Bramble is a from-scratch LoRa mesh protocol and firmware. Its primary target is ESP32-S3 hardware (Heltec V3, T-Deck Plus, Heltec V4), with an experimental nRF52840 + LR1110 port (Seeed Wio-WM1110 dev kit and Seeed SenseCAP T1000-E tracker). It prioritizes privacy-first design, dual-substrate routing (reactive AODV by default with an opt-in flood transport), authenticated traffic, confirmable delivery, end-to-end DMs, tiered reliability, and airtime management. The project includes firmware, a Go SDK (`bramble-go`), a CLI (`bramble-cli`), and a web companion app with an Electron desktop shell.

**Maturity:** Early but functional, a solo-developer project. Running stable on 3 ESP32-S3 boards, with working mesh, RPC, BLE, Wi-Fi, GPS, location sharing, mailbox store-and-forward, and a web companion app. The nRF52840 targets are experimental: they join the bench mesh with BLE RPC and provisioning over an encrypted link, plus a bench-verified GNSS driver on the T1000-E, but no power management. Not yet field-deployed at scale, and no user base beyond the developer's own bench.

---

## Architecture Comparison

| Feature | Meshtastic | MeshCore | Bramble |
| --- | --- | --- | --- |
| **Routing approach** | Managed flooding (nodes rebroadcast up to hop limit). Next-hop routing for DMs shipped in 2.6. Zero-Cost Hops (2.7.11): hops between favorited router-class nodes do not decrement the hop limit, so router backbones count as one hop. | Flooding for discovery and group channels; DMs are **source-routed**: the sender embeds the learned repeater path (1 byte per hop) in the packet, with flood fallback on retry. Client nodes never repeat; only repeaters and room servers forward. | Dual-substrate. Reactive AODV (default): RREQ/RREP discovery, cached routes, reverse-route breadcrumbs from DATA, intermediate-node RREP, route-forwarded ACKs. Opt-in flooding transport (`s_flood_transport`, default off): hop-limited, deduplicated, airtime-budget-gated floods with a flooded ACK. Channel/group messages relay multi-hop via the flood relay. |
| **Encryption: channels** | AES256-CTR with PSK. No integrity check (no AEAD: issue #4030 still open as of 2026-07). Known-plaintext forgery possible for any PSK holder. | **AES-128**-ECB with a 16-byte PSK plus an HMAC-SHA256 truncated to **2 bytes**. Hashtag channels derive the key as SHA-256("#name") truncated to 16 bytes, so the key is guessable from the channel name. | AES-256-GCM (AEAD) with PSK. Channel ID encrypted inside ciphertext. |
| **Encryption: DMs** | Since v2.5: per-node X25519 keys + AES-CCM. v2.7 added **Key Verification**: a six-digit out-of-band code both sides confirm to verify DM keys. Note CVE-2025-52464 (June 2025, CVSS 9.5): cloned/low-entropy keypairs from vendor golden images and weak RNG seeding allowed DM decryption; patched in 2.6.11/2.6.12, hardware RNG in 2.7.23. | Ed25519 identity converted to X25519 for ECDH, then AES-128-ECB encrypt-then-MAC with a 2-byte truncated MAC and zero padding. Signed adverts prevent advert spoofing. ECB mode and the 2-byte MAC are real weaknesses. | Per-peer end-to-end AES-256-GCM sessions, keyed by a role-symmetric quad-DH X25519 exchange with a 7-digit SAS (`components/dm_session`). Not readable by other channel members. Sessions ratchet: a per-direction HKDF chain gives per-message forward secrecy, and a DH ratchet folds fresh entropy in once per key-exchange epoch. The SAS-comparison UX ships in the web client, the T-Deck graphical build, and the pager (see [SECURITY-MODEL.md](SECURITY-MODEL.md)). |
| **Traffic authentication** | Channel traffic is unauthenticated: any PSK holder can forge/impersonate. DMs (v2.5+) are authenticated. | Per-node Ed25519 signatures on adverts; DM payload MACs are only 2 bytes. No relay-verified control-plane authentication. | DATA frames and the control plane (RREP, RERR, ACK, delivery receipt, beacon) carry a network-key HMAC that relays verify before acting. There is no public default key: an unprovisioned node is inert (fail-closed) and authenticates nothing until a real per-fleet key is provisioned. Residual: any network-key holder is an insider and can forge these MACs ([SECURITY-MODEL.md](SECURITY-MODEL.md)). |
| **Scalability target** | ~80-100 nodes was the historically quoted practical ceiling for chat; the 2.6/2.7 series added active mitigations: broadcast intervals auto-scale above 40 online nodes, multi-message batching (2.7.18), transmit-history persistence (2.7.20), Zero-Cost Hops router backbones, and official guidance to move big meshes to faster presets. DEF CON 33 ran 2,000+ nodes (degraded but alive). Channel messages still flood. | Repeater-only flooding bounds fan-out to the repeater population; DMs follow embedded source routes. Default flooded-advert interval raised from 12h to **47h** in v1.16 to cut background load. Protocol supports up to 64 hops. | Reactive DM forwarding is O(path_length), not O(N). Honest measured scale under the collision-modeled simulator (the source of truth), re-measured 2026-07-24 at the PHY firmware transmits on (the frequency plan's SF9/125 kHz): 30% delivery at 10 nodes, 10% at 50, 0-5% at 100 and 0% at 200, as a single channel saturates under control-plane load ([results/simulation-2026-07-honest-baseline.md](results/simulation-2026-07-honest-baseline.md)). The earlier collision-free "100% at 200 nodes" figures were retracted as sim artifacts. No field test has been run. |
| **Reliability model** | Optional ACKs. Basic retries. No backpressure protocol; congestion-motivated features exist (interval scaling, batching). | ACK mechanism; v1.16 added extended 6-byte ACKs (groundwork for multi-attempt delivery). Basic retry logic. No formal congestion protocol. | Three tiers (Broadcast/Normal/Critical). Exponential backoff retries with jitter, end-to-end ACKs, delivery receipts with relay path. Flow-control and congestion-signaling designs were removed unshipped; sender pacing comes from the per-tier airtime budget at the TX chokepoint and the bounded pending-ACK table. |
| **Airtime management** | Regional duty-cycle limits are enforced (EU regions: 10% hourly rolling-window cutoff stops TX when exceeded), plus the congestion auto-scaling above. No per-node airtime budget or per-message costing. | Operator-set duty-cycle enforcement (`set dutycycle`, v1.14.1+), flood-hop caps (`flood.max.*`), SF-tuned preambles, 47h advert default. No per-tier budgeting. | Enforced: every transmission passes one budget-gated TX path with real time-on-air costing, per-tier token buckets, and regional duty-cycle caps where the frequency plan requires them (EU868 1%). No transmit path bypasses the budget. The most comprehensive of the three, though all three projects ship some airtime controls. |
| **Time sync** | GPS-based or NTP via WiFi/MQTT. No mesh-internal time sync protocol. | Repeaters support GPS clock sync (v1.14.1+); clocks can be set from the companion app/CLI. No mesh-internal sync protocol. | Stratum-based mesh time sync via beacon fields (corroboration-gated). GPS optional. |
| **Hardware targets** | ESP32, nRF52, RP2040/RP2350, STM32WL, Linux. Dozens of boards, steady new-board cadence. | 50+ devices (ESP32/S3/C3, nRF52, RP2040); 80+ firmware binaries per release; OTA updates on nRF. | ESP32-S3 running targets (Heltec V3, T-Deck Plus, Heltec V4) plus an experimental nRF52840 + LR1110 port (Wio-WM1110 dev kit, SenseCAP T1000-E: mesh + BLE RPC, no power management). Narrow focus by design. |
| **Protocol overhead** | Protobuf-encoded. Header ~16 bytes + protobuf payload. | 1-byte header + 4-byte transport codes + 1-byte path length + 0-64-byte path; payload up to 184 bytes, max packet 255. No protocol-level CRC (relies on LoRa PHY). | 12-byte base header, compact binary (no protobuf/JSON). Authenticated DATA adds a 28-byte envelope prefix (src_addr, relay-mutable prev_hop, 8-byte auth HMAC) plus a 12-byte nonce and 16-byte GCM tag. |
| **Node identity** | Node ID is MAC-derived (not cryptographic; spoofable on channels), but since 2.5 every node also has a per-node X25519 keypair for DM encryption/signing, with six-digit Key Verification since 2.7. | Ed25519 public key per node; signed adverts. On-air source/destination IDs are 1-byte public-key-prefix hashes (roughly 1-in-256 collision), so the on-air ID is key-derived but trivially collidable. | Ed25519 identity key per node; address = `SHA-256(key)[0:4]`. Self-signed, relay-gated identity attestations; receivers TOFU-pin verified bindings and reject any attestation whose address does not derive from its own key, so claiming another node's address is cryptographically infeasible. Pinned identities gate the timesync quorum and DM key continuity. |
| **Membership / Sybil control** | None. Any device on the channel PSK is a member. | Per-node keys and a per-peer contact list / ACL: trust is decided locally by each node's operator adding contacts; repeater admin is password-based per-repeater ACL. No fleet membership authority; no Sybil-scarcity story. | Optional per-fleet **trust anchor**: an operator-held Ed25519 anchor endorses node identities with a signed cert (`docs/trust-anchor.md`). An anchored mesh pins ONLY peers carrying an anchor-signed cert, so un-admitted Sybils cannot join, corroborate the timesync quorum, or pass DM key-continuity. The anchor private seed is operator-held and offline (never on a node, never over RPC). Honest bound: this stops outsiders and un-admitted Sybils, NOT a compromised admitted insider; anchor custody is the trust root; certs are permanent in v1 (no active revocation short of re-anchor). |
| **Fleet delivery audit** | Optional per-message ACKs for DMs. No fleet membership concept, so there is no protocol notion of "everyone" to audit a broadcast against (consistent with its zero-config design). | ACKs and delivery reports per unicast; trust is per-node contact lists, so no node can enumerate a fleet to audit against. | Optional attested roll-call ([rollcall.md](rollcall.md)): an authenticated broadcast that members answer with Ed25519 identity-signed unicasts, building a per-member ledger on the initiator. An anchored mesh audits against the endorsed member set ("M of N expected, these missing"); an un-anchored mesh reports observed responders only. A signature proves an answer; absence proves nothing. Rate-limited on both initiator and member sides. Verified by host tests and simulation; no field deployment. |
| **Deployment planning** | Site Planner (site.meshtastic.org): terrain-model RF coverage prediction (ITM over SRTM terrain data), a mature propagation-planning tool neither of the other two projects has. | Community coverage-mapping and monitoring tools. | Digital twin ([digital-twin.md](digital-twin.md)): each node exports its observed topology (`bramble.exportTopology`: neighbors with per-link RSSI/SNR, routes, radio parameters), and the simulator's `twin` subcommand replays the deployment against the collision model for capacity (delivery rate vs offered load) and node-loss partition analysis. Complementary to RF prediction, not a substitute: the twin replays observed links at a moment in time and predicts nothing about nodes not yet placed. Results are simulation. |
| **Zero-hardware onboarding** | Web flasher; companion apps require a flashed device. | Web flasher; companion apps require a flashed device. | Browser playground ([playground.md](playground.md), `make playground`): the emulator boots the actual firmware binary as a small virtual fleet on a simulated radio ether, with a guided tour through provisioning (the fail-closed inert state included), multi-hop relay, SAS verification, and a delivery receipt. A browser fleet is not a field deployment. |
| **Max hops** | 7 max (default 3); Zero-Cost Hops make favorited-router hops free, stretching effective reach well past 7. | Protocol supports 64 (1-byte path hashes). Current firmware defaults: `flood.max.unscoped` 64 (effectively uncapped), `flood.max.advert` 8. | Reactive: expanding-ring discovery 4 then 8, max route depth 8. Flood transport: configurable 1..32, default 8. |

---

## Routing

### Meshtastic: Managed Flooding + Next-Hop DMs

Meshtastic uses **managed flood routing** for channel messages. Every forwarding node rebroadcasts (up to the hop limit), with a key optimization: before rebroadcasting, a node listens briefly to see if another node already rebroadcast the same packet and suppresses its own copy ("implicit ACK"). Node roles (CLIENT, CLIENT_MUTE, CLIENT_BASE, ROUTER, ROUTER_LATE) influence rebroadcast timing and priority.

**Next-hop DM routing (shipped in 2.6, Feb 2025):** after an initial flooded exchange, the sender learns which neighbor successfully relayed to the destination and sends subsequent DMs directly to that next hop, falling back to flooding on the last retry. Applies to unicast DMs only, not channel messages.

**Zero-Cost Hops (2.7.11, Nov 2025):** hops between mutually-favorited router-class nodes (ROUTER, ROUTER_LATE, CLIENT_BASE) do not decrement the hop limit, so a router backbone spans as a single logical hop; 300+ mile spans have been demonstrated. This materially changes the "7 hops max" reach story.

**Scalability implications:** channel messages are still O(N) transmissions per message. DMs are O(path_length) after route learning. Above ~40 online nodes the firmware auto-scales broadcast intervals; 2.7 added multi-message batching. Large meshes are officially advised onto faster modem presets. Congestion still degrades large meshes (DEF CON's 2,000+ node network was heavily congested), but real mitigations ship.

### MeshCore: Flood Discovery, Source-Routed Data

MeshCore floods **adverts** (identity/position broadcasts) and group-channel traffic through repeater nodes only; client (companion) nodes never repeat. DMs are **source-routed**: the sender embeds the full learned repeater path in the packet (1 byte per hop), learned from flooded adverts and delivery reports, with flood fallback when a path fails. Optimizations include duplicate detection via packet hashes, random backoff before rebroadcast, RSSI-based prioritization, and hop caps (`flood.max.*`).

**Scalability implications:** discovery floods are O(N_repeaters), and data to known destinations transmits once per path hop. The 47-hour default advert interval (raised from 12h in v1.16) keeps background flood load low. The role split (few repeaters, many non-repeating clients) bounds amplification in dense deployments.

### Bramble: Reactive AODV-Style Routing

Bramble uses **on-demand (reactive) routing** for DMs. Routes are discovered only when needed via Route Request (RREQ) / Route Reply (RREP) packets, then cached and reused:

1. Source broadcasts RREQ (this *does* flood, similar to Meshtastic)
2. Destination unicasts RREP back along the reverse path
3. Subsequent data packets follow the discovered route: only nodes on the path transmit
4. Routes are cached with active/stale timeouts and maintained via broken-link detection

**Channel/group messages** use controlled flooding (hop-limited, default 8, configurable 1..32) and relay multi-hop, since group messages inherently need to reach all members.

**Opt-in flood transport.** A runtime toggle (`s_flood_transport`, default off) can also route unicast DMs by the same hop-limited, deduplicated, budget-gated flood instead of reactive discovery, with a flooded ACK for route-free sender confirmation. It trades airtime for reach and is best-effort within the hop budget. Both substrates ship; the toggle selects one; reactive stays the default.

**Privacy enhancement:** RREQ packets carry a per-query pseudonym instead of the originator's address, so forwarded route requests do not identify who is asking. The destination address stays cleartext, and the first hop can still infer the originator (a fresh RREQ has hop_count 0); see [SECURITY-MODEL.md](SECURITY-MODEL.md) for the precise scope.

**Scalability implications:** route discovery floods once; after that DM traffic is O(path_length). Meshtastic DMs have been O(path_length) since 2.6, and MeshCore DMs are source-routed, so per-hop unicast delivery is table stakes among all three. Bramble's architectural distinctions are the cached bidirectional route state with broken-link maintenance, route-forwarded (non-flooded) ACKs, and the airtime-budgeted control plane. The tradeoff is complexity: route maintenance, broken-link detection, and the RREQ/RREP exchange add protocol overhead and implementation complexity.

---

## Privacy & Encryption

### Meshtastic

| Aspect | Details |
| --- | --- |
| **Channel encryption** | AES256-CTR with PSK. **No integrity check** (CTR without MAC). Anyone who knows the PSK can forge messages impersonating any node. Issue #4030 (AEAD) still open as of 2026-07. |
| **DM encryption (v2.5+)** | Per-node X25519 keys + AES-CCM. Real E2E encryption with sender authentication. v2.7 added six-digit Key Verification UX. |
| **Key incident (2025)** | CVE-2025-52464 (CVSS 9.5): vendor firmware cloning and weak RNG seeding produced duplicated/low-entropy keypairs, allowing DM decryption and remote-admin hijack. Fixed in 2.6.11/2.6.12 (deferred keygen, compromised-key wipe); hardware RNG in 2.7.23. |
| **Forward secrecy** | None (officially documented limitation): static X25519 keys; "harvest now, decrypt later" applies to captured DM traffic. |
| **Metadata leakage** | Header always cleartext: source node ID, destination node ID, hop count, packet ID. All relays and passive observers see who talks to whom. |
| **Key management** | PSK shared out-of-band (QR code). No rotation mechanism. The default "LongFast" channel key (`AQ==`) is public knowledge, and the installed base depends on it for zero-config first contact. |
| **Authentication** | Channel: none (any PSK holder can impersonate). DMs (v2.5+): authenticated. |

### MeshCore

| Aspect | Details |
| --- | --- |
| **Identity** | Ed25519 key pair per node. Adverts (name/position/public key) are signed, preventing advert spoofing. |
| **DM encryption** | Ed25519 keys converted to X25519 for ECDH, then **AES-128-ECB** encrypt-then-MAC with an HMAC-SHA256 truncated to **2 bytes**, zero padding. ECB mode (identical plaintext blocks produce identical ciphertext blocks) and the 2-byte MAC (1-in-65536 forgery) are genuine cryptographic weaknesses. |
| **Channel/room encryption** | AES-128-ECB with 16-byte PSK + 2-byte truncated MAC. Hashtag channels derive keys from the channel name (`SHA-256("#name")[0:16]`), so they are effectively public. Room servers are BBS-style store-and-forward spaces with history push. |
| **Forward secrecy** | None: deterministic per-contact shared secret, no ratcheting or ephemeral keys. |
| **Metadata leakage** | On-air source/destination are 1-byte public-key-prefix hashes (about 1-in-256 collision), plus the embedded source-route path in DMs. Observable by relays and passive listeners; short hashes give plausible deniability but also enable misdelivery/collision. |
| **Key management** | Users broadcast signed "adverts" carrying name, position, and public key; contacts are added manually per node. No automatic rotation. |

**Note:** MeshCore has an official protocol reference at docs.meshcore.io (packet format, payload format, companion/KISS protocols). The best independent crypto analysis is Jack Kingsman's "A Hitchhiker's Guide to MeshCore Cryptography" (Jan 2026), which the MeshCore column above follows.

### Bramble

| Aspect | Details |
| --- | --- |
| **Channel encryption** | AES-256-GCM (AEAD: provides both confidentiality and integrity). Channel ID is inside the ciphertext, so non-members cannot determine which channel a message belongs to. Keys derive deterministically from the channel passphrase, so a passphrase holder can compute every epoch key. |
| **DM encryption** | Per-peer end-to-end AES-256-GCM sessions, keyed by a role-symmetric quad-DH X25519 exchange (four X25519 DHs mixed via HKDF-SHA256) with a 7-digit SAS for out-of-band verification (`components/dm_session`). The handshake travels inside DATA envelopes (`app_type = APP_TYPE_KE`); the standalone `KEY_EXCHANGE` packet type was retired from the wire. Other channel members cannot read a DM. |
| **Forward secrecy** | DMs: yes, per message. Each direction has its own HKDF chain seeded from the handshake secret; every message derives a fresh message key and then advances the chain, so a key recovered from one message decrypts neither earlier nor later messages (`components/dm_session/dm_session.c`, `dm_ratchet_step`). A 3-byte epoch-plus-index ratchet header is bound into the AEAD as associated data, and a bounded skip window absorbs reordering before degrading to a re-handshake. Post-compromise recovery is coarser: the DH ratchet (`dm_ratchet_dh`) folds fresh entropy into the root key once per key-exchange epoch, not per message, so recovery from a state compromise is bounded by the epoch cadence. Channels: no. Epoch keys derive from the previous epoch's key and everything derives from the passphrase, so a passphrase holder can compute every epoch. Verified by host tests (`test/test_dm_ratchet.c`), not by a third-party audit. |
| **Metadata leakage** | The cleartext header carries the destination address, packet id, and flags, and DATA packets carry a cleartext 4-byte source address plus a relay-mutable prev_hop. RREQ sources are pseudonymized per query; channel ID is inside the ciphertext; LOCATION ciphertext is padded to a fixed size so it does not leak the sharing tier. |
| **Key management** | Auto-generated identity on first boot. No automatic key rotation; a channel epoch mechanism exists, and receivers catch up across epoch advances via rate-limited trial decryption. |
| **Authentication** | Channel/DM payloads: the AES-GCM auth tag (a channel tag is producible by any channel-key holder; a DM tag requires the per-peer session key). Control plane and DATA (RREP, RERR, ACK, delivery receipt, beacon, plus DATA's `auth_hmac`): network-key HMAC, verified by relays before acting. Residual: forgeable by any network-key insider. There is no public-PSK fallback: an unprovisioned node is inert (fail-closed) and authenticates nothing until a per-fleet key is provisioned. RREQ itself carries no MAC (a broadcast route request has no pre-shared per-flow context). |

### Summary

Bramble holds a payload-crypto edge over both: AEAD channels versus Meshtastic's unauthenticated CTR, and AES-256-GCM versus MeshCore's AES-128-ECB with 2-byte truncated MACs and guessable hashtag-channel keys. Distinct points:

1. **Authenticated traffic, no insecure bootstrap**: DATA and the control plane carry a network-key HMAC that relays verify before acting, so an outsider on a provisioned network cannot forge control/DATA frames, and cannot replay them at a node that has been up since it saw the original (the replay windows are RAM-only, so replay reopens across a reboot; see [SECURITY-MODEL.md](SECURITY-MODEL.md)). Meshtastic's channel traffic is unauthenticated, and MeshCore's payload MACs are 2 bytes. There is no public default key: an unprovisioned node is inert (fail-closed). Residual: a network-key insider can still forge (see [SECURITY-MODEL.md](SECURITY-MODEL.md)).
2. **Confirmed delivery**: acknowledged tiers report DELIVERED vs FAILED to the sender, a genuine edge over Meshtastic's fire-and-forget at low-to-moderate load. The flooded-confirmation half degrades under high load (airtime scales O(messages x nodes)), so it is not a high-load guarantee.
3. **End-to-end DMs**: per-peer quad-DH X25519 AES-256-GCM sessions with DM key continuity against pinned identities (a known peer showing up with a different long-term key is refused, not silently accepted). Ahead of MeshCore's AES-128-ECB/2-byte-MAC DMs on payload crypto. Versus Meshtastic: comparable in kind (both have per-node-key E2E DMs) and both ship an out-of-band verification UX (Meshtastic's six-digit Key Verification in 2.7, Bramble's 7-digit identity-bound SAS in the web client, the T-Deck build, and the pager); Bramble's AEAD strength, key-continuity gating, and per-message DM ratchet are ahead, against a project with years of deployment Bramble does not have.
4. **Metadata hygiene**: RREQ source pseudonymization and fixed-size, tiered LOCATION ciphertext. Meshtastic sends full source/dest IDs cleartext; MeshCore sends 1-byte key-hash IDs plus explicit source-route paths. Bramble data packets still carry a cleartext 4-byte source address.
5. **Channel privacy**: Bramble hides which channel a message belongs to. Meshtastic and MeshCore don't.

### Per-node identity

Per-node identity is where Bramble and MeshCore are closest, since it is
MeshCore's historical strong suit: MeshCore has per-node Ed25519 keys and
signed adverts; Bramble has per-node Ed25519 keys, signed attestations,
AND an address that is a hash of the signing key, so an insider cannot
claim another node's address at all. MeshCore's on-air IDs are 1-byte
public-key-prefix hashes: key-derived (unlike a raw MAC) but trivially
collidable, with no relay-verified address-key binding. Meshtastic's node
ID is an unauthenticated MAC-derived number on channels, though since 2.5
its nodes do carry per-node DM keys with a shipped verification UX (2.7).
Stated honestly for an UN-anchored Bramble mesh: identities are free to
mint and Sybil-with-fresh-identities is not closed; Bramble's pins are
RAM-only (reset on reboot, re-established by TOFU); and Bramble's DM
first contact remains TOFU-grade until the peer's attestation is pinned.
The optional trust anchor (next subsection) is what closes Sybil scarcity
on an anchored mesh; without it, no Sybil-scarcity claim is made.

Attestation is a prerequisite for trusted participation, with the
bootstrap-quorum race bounded: a node holding zero pins trusts
established peers only within a per-boot grace window (5 minutes), after
which an unpinned peer never corroborates the timesync quorum. There is
no unattested path into the gated trust decisions (the timesync quorum
after the grace, and DM key continuity), and every Sybil identity is a
visible, counted, airtime-costing attestation. Honest scope: this is a
bootstrap-race close plus a uniform-attestation prerequisite, a bounded
property. Full Sybil scarcity holds only on an ANCHORED mesh, via the
trust anchor below.

### Trust anchor

The optional trust anchor is a piece neither of the other projects has:
per-node cryptographic identity WITH explicit, fleet-wide membership
control. Meshtastic has no membership control at all. MeshCore has
per-node keys and a per-peer contact-list / ACL model, but trust is decided
locally at each node: there is no membership authority, no Sybil-scarcity
story, and no way to un-trust a node fleet-wide short of every peer editing
its own contacts. Anchored Bramble is per-node identity + explicit endorsed
membership + confirmed delivery: an operator holds one offline anchor
keypair per fleet, provisions its public key to each node, and signs a
permanent endorsement cert over each node's identity key. An anchored node
pins ONLY peers carrying an anchor-signed cert, so an outsider or an
un-admitted Sybil cannot get pinned, join the identity-gated timesync
quorum, or pass DM key-continuity. This is the NEW-SEC-4 close in
[SECURITY-MODEL.md](SECURITY-MODEL.md) section 5; the operator ceremony is
[trust-anchor.md](trust-anchor.md).

Honest bounds, stated plainly: the anchor protects against outsiders and
un-admitted Sybils, NOT a compromised ADMITTED insider (a node you endorsed
that is later captured stays a valid member; endorsement is admission
control, not behavioral trust). The anchor private seed is the trust root:
it is operator-held and offline (localStorage in the operator's browser,
never sent to a node or over any RPC), and whoever holds it can admit any
node. Certs are permanent in v1 with no active revocation: a compromised
endorsed node cannot be individually un-trusted fleet-wide short of a
re-anchor flag day. Anchoring is opt-in per fleet; an un-anchored mesh keeps
the free-to-mint identity model above.

---

## Reliability

### Meshtastic

- **ACKs:** Optional, per-message, DMs only. No ACK for channel messages.
- **Retries:** Basic retries. No exponential backoff protocol documented.
- **Flow control:** None. No backpressure protocol.
- **Delivery confirmation:** ACK tells sender the message arrived. No relay path information.
- **Congestion handling:** No explicit signaling, but real mitigations shipped: broadcast-interval auto-scaling above 40 online nodes, multi-message batching (2.7.18), transmit-history persistence (2.7.20), and implicit-ACK rebroadcast suppression.

### MeshCore

- **ACKs:** Supported for unicast; v1.16 added extended 6-byte ACKs as groundwork for multi-attempt delivery.
- **Retries:** Basic retry logic with collision-avoidance delays; flood fallback when a source route fails.
- **Flow control:** None documented. Clients not repeating acts as implicit load limiting.
- **Delivery confirmation:** ACK mechanism present; delivery reports also feed path learning.
- **Congestion handling:** No explicit protocol; 47h advert default, flood-hop caps, and operator duty-cycle settings bound background load.

### Bramble

- **ACKs:** Three tiers: Broadcast (no ACK), Normal (end-to-end ACK), Critical (ACK + delivery receipt with full relay path).
- **Retries:** Exponential backoff with ±25% jitter. Normal: 3 retries from a 2 s base. Critical: 8 retries from a 3 s base. A retry denied by the airtime budget burns the attempt, so a saturated mesh produces a visible FAILED status instead of a zombie pending message.
- **Flow control:** Designed and component-tested (per-destination sliding window with AIMD), but not yet wired into the live mesh path.
- **Delivery confirmation:** Critical tier includes delivery receipts with complete relay path: sender passively learns network topology.
- **Congestion handling:** The dedicated CONGESTION packet type was removed unshipped. What ships is indirect: per-tier airtime budgets shed lower-tier traffic first, and control traffic (routing, ACKs) has a reserved budget lane that data load cannot starve.

---

## Scalability

### Meshtastic

**Historically quoted ceiling: ~80-100 nodes for usable chat. Higher with the 2.6/2.7 mitigations, but channel flooding remains the bound.**

Channel messages still generate up to N rebroadcasts. The 2.6/2.7 series changed the picture: DMs are next-hop routed (O(path_length)), broadcast intervals auto-scale above 40 online nodes, messages batch, and Zero-Cost Hops let router backbones span without consuming hop budget. DEF CON 33 (Aug 2025) ran 2,000+ nodes on one network: heavily congested, but it functioned, which is a real-world stress datapoint no other LoRa mesh project has. Official guidance moves large meshes off LongFast to faster presets. The lack of per-node airtime budgeting still means chatty nodes degrade the channel for everyone.

### MeshCore

**Bounded by repeater population, not total devices.**

Only repeaters and room servers forward, so a 100-device mesh with 20 repeaters floods through 20 nodes, not 100. DMs follow embedded source routes (one transmission per path hop). The 47-hour default advert interval keeps background discovery load minimal. Large regional meshes (UK, Netherlands, US metros, Australia) run on this model. No formal published scale ceiling; congestion behavior at high simultaneous-sender counts is not documented.

### Bramble

Reactive DM traffic scales as O(path_length) after route discovery: one transmission per hop, plus the initial RREQ flood amortized across subsequent messages. Discovery uses an expanding ring (hop limit 4 first, 8 on retries); routes up to 8 hops deep are usable end-to-end. The token-bucket airtime budget caps each node's transmission time with per-tier sub-budgets, so no single node can monopolize the channel. Channel messages flood (hop-limited, multi-hop), and the opt-in flood transport (`s_flood_transport`, default off) can route DMs the same way when enabled, trading airtime for reach.

**Validation status:** scale behavior has not been validated in a real-world deployment; no multi-node field test has been run. The simulator runs the real protocol code over a collision-model radio layer (real time-on-air, collisions, capture, half-duplex, LBT) and is the source of truth. The honest baseline ([results/simulation-2026-07-honest-baseline.md](results/simulation-2026-07-honest-baseline.md), which supersedes the June numbers for planning), re-measured 2026-07-24 at the frequency plan's SF9/125 kHz: 30% message delivery at 10 nodes, 10% at 50, 0-5% at 100, and 0% at 200. A single channel saturates under control-plane load (RREQ discovery storms amplifying a beacon floor) well before traffic flows: control traffic alone offers 0.91 erlang at 50 nodes and 1.94 at 200, and the 200-node grid also spans 11 to 17 hops, beyond the 8-hop route ceiling. The earlier collision-free "100% at 200 nodes" numbers were retracted as sim artifacts. The design's scaling argument is a goal under active investigation, not a demonstrated property. Both Meshtastic and MeshCore have real-world scale evidence Bramble lacks.

---

## Resource Usage

| Resource | Meshtastic | MeshCore | Bramble |
| --- | --- | --- | --- |
| **RAM** | ~100-200 KB (varies by platform and features enabled; unofficial figures, not published) | Lightweight: "no dynamic allocation except during setup." Exact figures not published. | ~127 KB total (20 KB protocol, 5 KB app, 18 KB RTOS, 84 KB system). 60% headroom on ESP32-S3 (320 KB available). |
| **Flash** | ~1.5-2 MB (ESP32 with all features; unofficial) | Compact: prebuilt binaries for 50+ boards. Size not published. | ~1.75 MB projected (256 KB firmware, 512 KB ESP-IDF, OTA partitions). |
| **Battery life** | Good with sleep modes. nRF52 boards excel (~days to weeks). ESP32 boards: ~1-3 days typical with screen. | Low power focus; clients never repeat, saving energy. v1.15/1.16 shipped measured ESP repeater and nRF companion power reductions; companion auto-shutdown off external power. | Designed for ESP32 deep sleep (~10µA). Airtime budgeting inherently conserves battery. No real-world battery data. |
| **Platform breadth** | ESP32, nRF52, RP2040/RP2350, STM32WL, Linux: very broad | ESP32/S3/C3, nRF52, RP2040: 50+ devices | ESP32-S3 primary plus experimental nRF52840: narrow by design |

---

## Ecosystem & Maturity

| Aspect | Meshtastic | MeshCore | Bramble |
| --- | --- | --- | --- |
| **Community size** | Very large (~51k Discord, active subreddit, many YouTube creators) | Large and growing fast (claimed 30,000+ users in 80+ countries, 3.2k GitHub stars, Wikipedia page, regional mesh orgs) | Solo developer project |
| **Companion apps** | Android, iOS, Web, Python CLI, extensive third-party tools | Android, iOS, Web, Windows/Mac/Linux desktop (closed-source freemium), meshcore.js, Python `meshcore-cli`, Home Assistant integration | Web companion app, Electron desktop app, Go SDK, CLI tool |
| **Hardware support** | Dozens of boards across six architectures | 50+ boards, 80+ binaries per release, nRF OTA updates | 3 running ESP32-S3 boards (Heltec V3, T-Deck Plus, Heltec V4) plus 2 experimental nRF52840 devices (Wio-WM1110 dev kit, SenseCAP T1000-E) |
| **Documentation** | Excellent. Official docs, community guides, YouTube tutorials. | Official docs site (docs.meshcore.io) with a protocol reference (packet/payload/companion/KISS); community sites; independent crypto analysis exists. | Protocol spec, architecture doc, code-verified security model, RPC reference, API docs |
| **Governance / licensing** | GPL firmware; Meshtastic LLC holds trademark; Meshtastic Solutions Inc. (2025) provides commercial backing and vendor partnerships. | Core firmware MIT (copyright Scott Powell / rippleradios.com). April 2026 governance split: the open project moved to meshcore-dev/meshcore.io; an unaffiliated proprietary fork (MeshOS, paid per-device licenses) operates from meshcore.co.uk. Companion apps closed-source freemium. | MIT-style single-owner repo. No trademark, no commercial entity. |
| **Production readiness** | Yes. Deployed in real emergencies, events, and daily use worldwide; 2,000+ node stress event. | Shipping for daily use in large regional meshes; vendors preload it on hardware. Less battle-tested than Meshtastic at extreme scale. | Pre-production. Functional firmware on dev boards, not field-tested at scale. |
| **Protocol spec** | Documented (mesh-algo page, protobuf definitions) | Official protocol reference at docs.meshcore.io (no RFC-style versioned spec) | Detailed design doc with packet formats, algorithms, pseudocode. |
| **MQTT/Internet bridge** | Yes, built-in; note the project-hosted public broker was locked down in 2024 (no all-topic subscription, position precision limited). Self-hosted brokers unaffected. | Not a core feature; community tools (MeshMonitor, meshcore-ha, LetsMesh) fill the gap. | Not planned initially. |
| **Web flasher** | Yes (flasher.meshtastic.org) | Yes (flasher.meshcore.io; the .co.uk flasher belongs to the MeshOS fork post-split) | Yes (browser-based flasher in `web-flasher/`, hosted at bramblemesh.org) |

---

## Why Bramble?

### Where Bramble Aims to Improve

1. **Routing scalability, honestly bounded.** Reactive routing (the default) keeps DM traffic at O(path_length) instead of O(N), the core architectural bet. Two honest caveats. First, the payoff has not materialized at scale yet: a single channel is still at capacity on control traffic alone by ~50 nodes (see Scalability above), so this is a design argument under investigation, not a demonstrated win. Second, Meshtastic DMs are next-hop routed since 2.6 and MeshCore DMs are source-routed, so O(path_length) unicast is not a differentiator by itself. What is distinct is the maintained bidirectional route cache, route-forwarded ACKs, and budget-gated control plane.

2. **Privacy by design.** Bramble's pseudonymized RREQ sources and encrypted channel IDs represent a meaningfully different privacy posture; [SECURITY-MODEL.md](SECURITY-MODEL.md) documents exactly what is and is not hidden. Meshtastic carries full source/destination IDs in plaintext headers; MeshCore carries 1-byte key-hash IDs plus explicit source-route paths.

3. **Airtime economics.** The token-bucket airtime budget with per-tier sub-budgets, enforced at a single TX choke point with real time-on-air costing, is the most comprehensive airtime model of the three, though not the only one: Meshtastic enforces regional duty-cycle cutoffs and auto-scales broadcast intervals under load, and MeshCore ships operator duty-cycle settings and flood caps. Bramble's distinction is per-message costing against per-tier budgets with a reserved control-plane lane, not the existence of airtime controls.

4. **Reliability tiers.** Three tiers with different retry strategies, end-to-end ACKs, and path-tracing delivery receipts give applications fine-grained control (sliding-window flow control and congestion signaling were removed unshipped). Meshtastic offers a simpler model (optional ACKs with fixed retries); MeshCore's extended ACKs (v1.16) are early groundwork in this direction.

5. **Authenticated traffic and AEAD payloads.** AES-256-GCM gives channel and DM payloads confidentiality and integrity in one pass. Meshtastic's AES256-CTR channels lack integrity entirely; MeshCore's AES-128-ECB with 2-byte truncated MACs is weak against both pattern leakage and forgery. A network-key HMAC on DATA and the control plane lets relays reject forged frames from outsiders, and replayed ones for as long as the receiver has been up since it saw the original (replay state is RAM-only and resets on reboot). The honest limit: a network-key insider can still forge (narrowed by per-node identity, not by provisioning).

6. **Confirmable delivery.** Acknowledged tiers report DELIVERED vs FAILED rather than fire-and-forget. This is a real edge at low-to-moderate load; the flooded-confirmation half degrades under high message load (airtime scales O(messages x nodes)), so it is bounded, not a high-load guarantee.

7. **End-to-end DMs.** Per-peer quad-DH X25519 AES-256-GCM sessions, so channel members cannot read each other's DMs. Sessions ratchet per message, so DM traffic has forward secrecy; post-compromise recovery is per key-exchange epoch, not per message. Stronger payload crypto than MeshCore's AES-128-ECB DMs; comparable in kind to Meshtastic's PKC DMs, and both projects ship an out-of-band verification UX (Meshtastic's six-digit Key Verification in 2.7, Bramble's identity-bound 7-digit SAS in the web client, the T-Deck build, and the pager). Bramble's ratchet is verified by host tests only, with no third-party audit and no field deployment behind it.

8. **No insecure bootstrap, no public default key (greenfield property).** Bramble ships with no well-known default network key: an unprovisioned node is inert (fail-closed) and refuses to emit or accept a single authenticated control/DATA frame until it holds a real per-fleet key, either minted on-device as a fleet founder key or pasted/joined out of band. This is a structural advantage a greenfield project can take that Meshtastic cannot: Meshtastic ships the well-known public "LongFast" default channel key (`AQ==`) that its entire installed base relies on for zero-config first contact, so it must keep that publicly-known default key for backward compatibility. (MeshCore's hashtag channels have the same class of problem: keys derived from guessable channel names.) Honest boundaries: (a) this is control-plane *authentication*, not confidentiality of Bramble's own opt-in public broadcast channel (`components/channel/public_channel.c`), which is a deliberate unauthenticated-to-everyone feature with a public key and is NOT a control-plane default; and (b) it excludes non-members, not misbehaving members: a network-key insider can still forge control MACs (addressed by per-node Ed25519 identity, not by this provisioning work). See [SECURITY-MODEL.md](SECURITY-MODEL.md).

9. **Fleet-auditable delivery (attested roll-call).** On top of the trust anchor and the receipt machinery, an initiator can ask "who is reachable right now, and prove it": an authenticated broadcast that each member answers with an identity-signed unicast, accumulating a per-member ledger with relay paths. This composes three pieces the other projects do not have together (enumerable membership, identity-signed answers, receipt plumbing); Meshtastic has no fleet membership concept to audit against, and MeshCore's per-node contact lists mean no node can enumerate a fleet, in both cases a deliberate design difference rather than an oversight. Honest bounds: answers cost N unicasts per roll-call so the primitive is rate-limited on both sides, absence never proves anything, and the whole feature is host-test and simulation verified with no field deployment ([rollcall.md](rollcall.md)).

10. **Deployment introspection (digital twin).** Because the simulator runs the real protocol code under a collision-modeled radio, an operator can export each node's observed topology over RPC and replay their actual deployment: where the capacity knee is, and which single node's loss partitions the mesh. Meshtastic's Site Planner answers a different and complementary question (terrain-based RF coverage for nodes you have not placed yet) and is the more mature planning tool; the twin only replays links that were observed, and its results are simulation ([digital-twin.md](digital-twin.md)).

11. **Zero-hardware onboarding (browser playground).** The emulator boots the actual firmware binary as a virtual fleet in a browser with a guided tour through provisioning, multi-hop relay, SAS verification, and delivery receipts, so the first contact with Bramble needs no hardware and no toolchain. All three projects have web flashers; a full product experience before owning a device is a structural benefit of the emulator running the real firmware, and a browser fleet is still not a field deployment ([playground.md](playground.md)).

### Where Bramble Is at a Disadvantage

1. **It's early.** Meshtastic has thousands of deployed nodes, years of real-world testing, and a thriving community. MeshCore is past the early-adopter stage: v1.16, 50+ devices, tens of thousands of users, and vendors preloading it on hardware. Bramble has working firmware on a handful of dev boards but no field deployments or community. The gap between a working prototype and a production mesh network is substantial.

2. **Reactive routing complexity.** AODV-style routing is well-understood in theory but tricky in practice over lossy LoRa links. Route discovery adds latency to the first message. Route maintenance (broken links, stale routes) adds implementation complexity. Meshtastic's managed flooding "just works", and MeshCore's source-routing keeps per-node state minimal (the sender carries the path).

3. **Hardware breadth.** Three running ESP32-S3 boards, plus an experimental nRF52840 + LR1110 port (Wio-WM1110 dev kit, SenseCAP T1000-E) that joins the mesh over BLE RPC but has no power management, so the nRF52 battery-life advantage (the best in the LoRa ecosystem, and a mature OTA-updatable target for both Meshtastic and MeshCore) is not realized. No Linux deployment target. Meshtastic runs on six architectures; MeshCore ships 80+ binaries.

4. **Small ecosystem.** Web app, desktop app, Go SDK, and CLI exist, but no mobile apps, no MQTT bridge, no community contributors. Building an ecosystem from one developer is a significant challenge.

5. **Single developer.** Both Meshtastic and MeshCore benefit from community contributions and diverse perspectives (though MeshCore's 2026 governance split shows multi-party projects carry their own risks). A solo project carries bus-factor risk and limited testing capacity.

6. **Channel traffic and flooded confirmation are airtime-bound.** Channel/group messages relay multi-hop by controlled flooding, so the reactive routing advantage applies mainly to DMs. The flood-based confirmed-delivery mode's airtime scales as O(messages x nodes): it holds confirmation only at low-to-moderate load and collapses under sustained high load. That load ceiling is close to fundamental to flooding, not a tuning bug.

7. **First-message latency.** Route discovery takes time: potentially 5-15 seconds for the RREQ/RREP round trip before the first DM can be sent. Meshtastic's flooding delivers (or drops) immediately; a MeshCore sender with a known path transmits immediately too. For time-sensitive first contact, this matters.

---

## Key sources (verified 2026-07-08)

- Meshtastic releases: <https://github.com/meshtastic/firmware/releases> (2.7.26 Beta, 2026-06-24)
- Meshtastic mesh algorithm and encryption docs: <https://meshtastic.org/docs/overview/mesh-algo/> , <https://meshtastic.org/docs/overview/encryption/>
- Meshtastic Site Planner (verified 2026-08-08): <https://meshtastic.org/docs/software/site-planner/> , <https://site.meshtastic.org/>
- Zero-Cost Hops: <https://meshtastic.org/blog/zero-cost-hops-favorite-routers/>
- CVE-2025-52464: <https://github.com/meshtastic/firmware/security/advisories/GHSA-gq7v-jr8c-mfr7>
- Channel AEAD issue (open): <https://github.com/meshtastic/firmware/issues/4030>
- MeshCore repo and releases: <https://github.com/meshcore-dev/MeshCore> (v1.16.0, 2026-06-06)
- MeshCore official docs: <https://docs.meshcore.io/>
- MeshCore v1.16.0 / v1.15.0 notes: <https://blog.meshcore.io/2026/06/06/release-1-16-0> , <https://blog.meshcore.io/2026/04/19/release-1-15-0>
- MeshCore governance split: <https://blog.meshcore.io/2026/04/23/the-split>
- MeshCore crypto analysis: <https://jacksbrain.com/2026/01/a-hitchhiker-s-guide-to-meshcore-cryptography/>
