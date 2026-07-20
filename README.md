<p align="center">
  <img src="assets/bramble-logo.png" alt="Bramble" width="160" height="160">
</p>

<h1 align="center">Bramble</h1>

<p align="center">
  Privacy-first LoRa mesh networking for ESP32-S3, built with ESP-IDF.
</p>

<p align="center">
  <a href="https://github.com/justinlindh/bramble/actions/workflows/quality.yml"><img alt="Quality" src="https://github.com/justinlindh/bramble/actions/workflows/quality.yml/badge.svg?branch=main"></a>
  <a href="https://github.com/justinlindh/bramble/actions/workflows/firmware-quality.yml"><img alt="Firmware Quality" src="https://github.com/justinlindh/bramble/actions/workflows/firmware-quality.yml/badge.svg?branch=main"></a>
  <a href="https://github.com/justinlindh/bramble/actions/workflows/webapp-quality.yml"><img alt="Webapp Quality" src="https://github.com/justinlindh/bramble/actions/workflows/webapp-quality.yml/badge.svg?branch=main"></a>
  <a href="LICENSE"><img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-blue.svg"></a>
  <img alt="Hardware: verified" src="https://img.shields.io/badge/hardware-verified-brightgreen.svg">
</p>

Bramble is an encrypted, multi-hop mesh protocol and firmware stack for long-range, infrastructure-free communication. It is designed for resilient field use while reducing metadata exposure; [docs/SECURITY-MODEL.md](docs/SECURITY-MODEL.md) documents exactly what is and is not protected.

## Table of Contents

- [What is Bramble?](#what-is-bramble)
- [What Makes Bramble Different](#what-makes-bramble-different)
- [Web Client](#web-client)
- [Hardware Targets](#hardware-targets)
- [Simulator](#simulator)
- [Emulator](#emulator)
- [Getting Started](#getting-started)
- [Testing](#testing)
- [Architecture](#architecture)
- [Documentation](#documentation)
- [API and SDK](#api-and-sdk)
- [CI/CD](#cicd)
- [Status](#status)
- [Contributing and Community](#contributing-and-community)
- [License](#license)

## What is Bramble?

Bramble is a secure LoRa mesh protocol and firmware implementation for ESP32-S3 devices with SX1262 radios. It focuses on practical private communications in constrained, lossy RF environments, with explicit handling for routing, retransmission, airtime limits, and node identity.

The firmware currently runs on real hardware (including T-Deck Plus and Heltec V3/V4 targets), with host-side testing used to validate protocol behavior continuously during development.

## What Makes Bramble Different

Compared to Meshtastic and MeshCore-style systems, Bramble leans on privacy and on authenticated, confirmable delivery at the protocol level. It is an early project: functional and hardware-verified on a small bench mesh, but not yet field-tested at scale. The claims below are what the code on `main` does, with the honest limits stated alongside.

- **Dual-substrate routing.** Two forwarding substrates ship, and a runtime toggle (`s_flood_transport`, default off) selects between them. **Reactive AODV is the default:** RREQ/RREP discovery, a cached route table, reverse-route breadcrumbs learned from DATA, intermediate-node RREP, and route-forwarded ACKs. DM airtime is `O(path_length)`, not `O(N)`. **Flooding is the opt-in alternative:** hop-limited (configurable, default 8, range 1..32), deduplicated, airtime-budget-gated unicast/channel floods with a flooded ACK for route-free sender confirmation. Reactive is not going away; the toggle picks one.
- **Authenticated traffic (wire v4), no insecure bootstrap.** DATA frames and the control plane (RREP, RERR, ACK, delivery receipt, beacon) carry a network-key HMAC that relays verify before acting; a captured frame cannot be forged or replayed by an outsider on a provisioned network. There is no public default key: an unprovisioned node is inert (fail-closed) and refuses to emit or accept any authenticated frame until a real per-fleet key is provisioned (minted on-device or pasted in). Residual: any network-key holder is an insider and can still forge these MACs, narrowed by per-node identity rather than by provisioning ([docs/SECURITY-MODEL.md](docs/SECURITY-MODEL.md)).
- **Confirmed delivery.** Acknowledged tiers report DELIVERED vs FAILED to the sender rather than fire-and-forget. Honest envelope: reactive holds confirmed delivery at small, dense scale; the flooded confirmation holds at low-to-moderate message load and **degrades at high load** because flood airtime scales as `O(messages x nodes)`. It is explicitly not a high-load guarantee.
- **End-to-end direct messages.** DMs use per-peer AES-256-GCM sessions keyed by a role-symmetric quad-DH X25519 exchange (`components/dm_session`), and are not readable by other channel members. Each message now has forward secrecy from a symmetric HKDF ratchet, with a coarse per-epoch DH ratchet for post-compromise recovery. An identity-bound 7-digit safety number (SAS) lets two people verify a contact out of band; the verification UX ships in the web client, the T-Deck graphical build, and on the e-paper pager itself (a per-peer view under the Nodes screen with a two-step confirm), and verified state is remembered per contact and cleared automatically if a contact's identity key changes. See [docs/SECURITY-MODEL.md](docs/SECURITY-MODEL.md) for the threat model and residuals.
- **Channel encryption + multi-hop channels.** Channel messages use AES-256-GCM (AEAD) with the channel ID hidden inside the ciphertext, and they now relay multi-hop via the flood relay rather than being single-hop only.
- **Privacy-first routing.** Route-request sources are pseudonymized per query, so forwarded route requests do not carry the originator's address.
- **Airtime budgeting:** every transmission goes through one budget-gated TX path (`components/radio/tx_gate.c`) with real time-on-air costing, per-tier token buckets, and regional duty-cycle caps enforced where the frequency plan requires them (for example EU 868 at 1%).
- **3-tier reliability model:** Broadcast (fire-and-forget), Normal (acknowledged, 3 retries with backoff), and Critical (8 retries plus delivery receipts carrying the relay path).
- **Cryptographic node identity + optional fleet trust anchor.** Each node has an Ed25519 identity whose 4-byte address is the hash of its own public key (`SHA-256(ed25519_pub)[0:4]`), so an address cannot be claimed without the matching key: address impersonation is a preimage search, not a TOFU race. Nodes flood self-signed identity attestations that peers verify and TOFU-pin, refusing and counting any later conflicting key for a pinned address. An optional per-fleet trust anchor (an operator-held Ed25519 key that endorses each member's identity with a cert) closes Sybil identity minting on an anchored mesh, because an anchored node only pins peers carrying a cert its anchor signed. Residuals stated in the same breath: a compromised endorsed insider stays a valid member (endorsement is admission control, not behavioral trust), the anchor seed is the fleet's trust root and its custody is the whole scheme, and an un-anchored mesh keeps unforgeable but free-to-mint identities ([docs/trust-anchor.md](docs/trust-anchor.md), [docs/SECURITY-MODEL.md](docs/SECURITY-MODEL.md)).
- **Store-and-forward mailbox:** offline nodes receive queued messages when they rejoin the mesh.
- **Location sharing with privacy tiers:** presence, zone (coarse ~1km), or exact; per-peer control over what you share and with whom. Location payloads are now AES-256-GCM encrypted end to end and padded to a fixed size so the ciphertext does not leak which tier was chosen; timing and the fact that a node shares location remain observable ([docs/SECURITY-MODEL.md](docs/SECURITY-MODEL.md)).
- **Browser-based flashing:** flash firmware to new devices directly from the web, no toolchain required.

For a deeper feature-by-feature analysis, see [docs/COMPARISON.md](docs/COMPARISON.md).

## Web Client

Bramble includes a web client for live network operation and monitoring. It provides real-time chat (including delivery badges), map-based peer location views, neighbor and route visualization, traffic monitoring, channel management, and radio configuration.

![Chat](docs/images/webapp-chat.png)
![Nodes](docs/images/webapp-nodes.png)
![Map](docs/images/webapp-map.png)
![Stats](docs/images/webapp-stats.png)
![Config](docs/images/webapp-config.png)

See [docs/webapp/chat.md](docs/webapp/chat.md) for current web client behavior and usage notes. To run it from source, `cd webapp && npm ci && npm run dev`; [webapp/README.md](webapp/README.md) covers the dev workflow, the WiFi-transport backend, and the Electron builds.

A desktop app (Linux, Windows, macOS) is also available, built from the same webapp. It connects directly to nodes on your local network instead of going through a hosted proxy, and can discover nodes on the LAN automatically. See [docs/webapp/desktop.md](docs/webapp/desktop.md).

## Hardware Targets

| Board | MCU | Display | Input | Radio | Audio | Status |
| ------ | ----- | --------- | ------- | ------- | ------- | -------- |
| Heltec WiFi LoRa 32 V3 | ESP32-S3 | 0.96" SSD1306 OLED (128x64) | Buttons | SX1262 | N/A | Running target |
| Heltec WiFi LoRa 32 V4 | ESP32-S3 | OLED + optional L76K GNSS | Buttons | SX1262 | N/A | Running target |
| LilyGo T-Deck Plus | ESP32-S3 | ST7789 320x240 LCD with LVGL v9 UI | GT911 capacitive touch + I2C keyboard | SX1262 with TCXO (DIO3 1.8V, DC-DC) | I2S with NVS-persisted volume | Running target with full GUI |
| Bramble Pager v1 (custom PCB) | ESP32-S3-WROOM-1 | 2.13" GDEY0213B74 e-paper (SSD1680, 250x122) | Buttons | SX1262 with TCXO and DIO2 RF switch (DC-DC) | Buzzer + vibra alerts | Design complete, boards not yet ordered |

The Bramble Pager v1 is a custom in-house board, not an off-the-shelf dev kit: its own 96x50mm 2-layer PCB (ESP32-S3-WROOM-1 + SX1262 with a DIO2 RF switch, GDEY0213B74 2.13" e-paper, ATGM336H GPS, buzzer, vibration motor, and status LED), JLCPCB-fabbable, in a 3D-printed case. The authoritative design tree (spec, schematic, PCB, BOM, case, and bring-up gates) is [hardware/pager/v1/](hardware/pager/v1/); build it with the `bramble-pager` board profile.

## Simulator

Bramble ships with a mesh simulator that runs real protocol code against a virtual radio layer and renders topology/traffic in a browser. The radio layer models the shared LoRa medium (real time-on-air, collisions, capture, half-duplex, listen-before-talk), making it the primary proving ground for scale and routing behavior before field deployment.

See [simulator/README.md](simulator/README.md) for setup and scenarios. For measured scale results under the collision model, [docs/results/simulation-2026-07-honest-baseline.md](docs/results/simulation-2026-07-honest-baseline.md) is the current source of truth (it supersedes the June numbers for planning; the earlier collision-free "100% at 200 nodes" figures were retracted as sim artifacts). In short: about 95% delivery at 10 nodes, collapsing to roughly 10-12% at 50-100 nodes and 0% at 200 as a single SF10 channel saturates under control-plane load. Scale is bounded by radio profile, node density, and hop budget; there is no field-tested-at-scale result.

## Emulator

The simulator drives real protocol code through a test harness; the emulator goes one step further and runs the **actual firmware binary**. Each virtual node is `app_main` compiled for ESP-IDF's linux target, booted with a virtual board profile, with the radio, e-paper display, buttons, GPS, and battery replaced by virtual drivers at the existing hardware seams. N virtual Bramble Pagers attach to the gosim ether and are rendered in a browser as the physical device: a faithful SSD1680 e-paper panel, clickable buttons, per-node consoles, and true per-node identities that survive restart. It is the packaging/UX proving ground the simulator is not: what a user actually sees and does on the device.

Quick start: `cd emulator && make run` (local toolchain) or `docker compose up --build` (zero prerequisites), then open the printed URL and load the `emu-channel-delivery` scenario. A gated PHY-passthrough bridge lets a serial-attached real node inject the physical RF channel into the virtual ether. See [emulator/README.md](emulator/README.md) for setup, scenarios, and the headless CI/E2E suites. A QEMU true-VM backend (running the exact flashable image) is a documented phase-2 follow-on, not yet built.

## Getting Started

Full build/flash instructions (including board-specific profiles and USB-port notes) are in [docs/BUILDING.md](docs/BUILDING.md).

Quick start (building from source needs ESP-IDF v5.4.1; see
[docs/BUILDING.md](docs/BUILDING.md) to install and activate it):

```bash
git clone https://github.com/justinlindh/bramble.git
cd bramble

# activate ESP-IDF in this shell first
source "$IDF_PATH/export.sh"

bash scripts/flash.sh local heltec-v3 build
bash scripts/flash.sh local heltec-v3 flash /dev/ttyUSB0
```

Use `tdeck-plus` instead of `heltec-v3` for T-Deck Plus builds.

No toolchain, no problem: the [web flasher](https://bramblemesh.org/web-flasher/)
flashes a device over USB straight from the browser. If a build or flash
stalls (serial permissions, a missing toolchain, a port collision), see
[docs/troubleshooting.md](docs/troubleshooting.md).

**First-time setup.** A freshly flashed node boots unprovisioned and inert: it has no network key, so it will not mesh (it neither emits nor accepts authenticated control-plane traffic) until you provision one, and the web client shows a prominent UNPROVISIONED banner until then. Follow [docs/getting-started.md](docs/getting-started.md) to connect the web client, provision a network key, optionally enroll the node under a trust anchor, and send your first message.

## Testing

Host-side tests cover crypto, routing, security, packet handling, reliability, and integration behavior.

```bash
cd test
./run_all_tests.sh
```

## Architecture

Bramble is organized as ESP-IDF components with clear boundaries between protocol logic, radio abstraction, security, reliability, and UI/control layers. The architecture is designed so core protocol behavior can be validated on host and simulator environments before device rollout.

For the full component breakdown and interaction diagrams, see [docs/bramble-architecture.md](docs/bramble-architecture.md).

## Documentation

- [docs/README.md](docs/README.md): documentation index (recommended entry point)
- [CONTRIBUTING.md](CONTRIBUTING.md): contributor setup, quality gates, and conventions
- [docs/troubleshooting.md](docs/troubleshooting.md): common build, flash, and test stalls
- [docs/BUILDING.md](docs/BUILDING.md): build, flash, monitor workflows
- [docs/bramble-architecture.md](docs/bramble-architecture.md): component-level architecture
- [docs/SECURITY-MODEL.md](docs/SECURITY-MODEL.md): threat model, verified protections, and known gaps
- [docs/auth.md](docs/auth.md): RPC authentication (on by default), pairing, and the browser origin allowlist
- [docs/COMPARISON.md](docs/COMPARISON.md): comparison with other mesh systems
- [docs/bramble-protocol-spec.md](docs/bramble-protocol-spec.md): protocol details
- [docs/bramble-testing.md](docs/bramble-testing.md): test strategy and coverage
- [docs/ota-rollout.md](docs/ota-rollout.md): OTA operator workflow
- [docs/quality-policy.md](docs/quality-policy.md): repo-wide CI gates, promotion criteria, and rollback levers
- [docs/quality-policy-webapp.md](docs/quality-policy-webapp.md): webapp workflow required/advisory mapping, local parity commands, and rollback levers
- [simulator/README.md](simulator/README.md): simulator usage
- [docs/webapp/chat.md](docs/webapp/chat.md): web client chat and UX notes

## API and SDK

Bramble exposes a JSON-RPC 2.0 interface for device control and observability.

- [api/openapi.yaml](api/openapi.yaml): OpenAPI spec for the RPC surface (synced to the firmware registry, CI-enforced by `scripts/check-rpc-contract.sh`; see [VERSIONING.md](VERSIONING.md))
- [bramble-go](https://github.com/justinlindh/bramble-go): Go SDK (serial, WebSocket, BLE)
- [bramble-cli](https://github.com/justinlindh/bramble-cli): CLI/TUI built on bramble-go
- [VERSIONING.md](VERSIONING.md): compatibility matrix

## CI/CD

- [.github/workflows/quality.yml](.github/workflows/quality.yml) runs the umbrella required gates: host tests, static checks, gosim integration, webapp checks, web-flasher tests, and the emulator suite.
- [.github/workflows/firmware-quality.yml](.github/workflows/firmware-quality.yml) runs firmware quality gates: required clang-format/shellcheck/actionlint.
- [.github/workflows/webapp-quality.yml](.github/workflows/webapp-quality.yml) runs webapp required gates (lint/typecheck/unit/build/e2e smoke).
- [.github/workflows/release-components.yml](.github/workflows/release-components.yml) publishes per-component releases and attaches firmware factory images and desktop installers.
- Additional workflow definitions live in [.github/workflows](.github/workflows).

## Status

Bramble is an early but fully functional project: the protocol stack is implemented end to end, reviewed and host-tested, and running stable on real hardware today (T-Deck Plus, Heltec V3, and Heltec V4). Every change must pass the full host test suite as a required CI gate (`test/run_all_tests.sh`, which fails if any suite fails or none are found). It has not yet been field-tested at scale, and development is ongoing.

## Contributing and Community

Contributions are welcome, and you do not need hardware to make one. The
webapp, the simulator, the host test suites, and the docs all build and run on
an ordinary machine.

- **[CONTRIBUTING.md](CONTRIBUTING.md)**: setup, the quality gates, branch and
  commit conventions, and what a good pull request looks like. Start there.
  Note the one step that is easy to miss: run `make setup-hooks` once per
  clone so the pre-commit checks run on your machine instead of failing in CI.
- **[docs/troubleshooting.md](docs/troubleshooting.md)**: serial permissions,
  toolchain targets, port collisions, and the other common first-run stalls.
- **[Issues](https://github.com/justinlindh/bramble/issues)**: bug reports and
  feature requests.
- **[Discussions](https://github.com/justinlindh/bramble/discussions)**:
  questions, ideas, and show-and-tell.
- **[SECURITY.md](SECURITY.md)**: how to report a vulnerability privately.
  Please do not open a public issue for one.
- **[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)**: what is expected of everyone
  taking part.

GitHub is canonical for outside contributions. Any other remote you may come
across is a mirror.

## License

MIT; see [LICENSE](LICENSE)
