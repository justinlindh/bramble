# Bramble

Privacy-first LoRa mesh networking for ESP32-S3, built with ESP-IDF.

Bramble is an encrypted, multi-hop mesh protocol and firmware stack for long-range, infrastructure-free communication. It is designed for resilient field use while minimizing metadata exposure: not just message contents, but key routing details are protected as well.

## Table of Contents

- [What is Bramble?](#what-is-bramble)
- [What Makes Bramble Different](#what-makes-bramble-different)
- [Web Client](#web-client)
- [Hardware Targets](#hardware-targets)
- [Simulator](#simulator)
- [Getting Started](#getting-started)
- [Testing](#testing)
- [Architecture](#architecture)
- [Documentation](#documentation)
- [API and SDK](#api-and-sdk)
- [CI/CD](#cicd)
- [Status](#status)
- [License](#license)

## What is Bramble?

Bramble is a secure LoRa mesh protocol and firmware implementation for ESP32-S3 devices with SX1262 radios. It focuses on practical private communications in constrained, lossy RF environments, with explicit handling for routing, retransmission, airtime limits, and node identity.

The firmware currently runs on real hardware (including T-Deck Plus and Heltec V3/V4 targets), with host-side testing used to validate protocol behavior continuously during development.

## What Makes Bramble Different

Compared to Meshtastic and MeshCore-style systems, Bramble prioritizes privacy and scalability at the protocol level:

- **Privacy-first routing:** route-request source addresses are encrypted to reduce metadata leakage.
- **Reactive AODV routing:** direct message delivery scales with path length (`O(path_length)`) rather than flooding to all nodes (`O(N)`).
- **AES-256-GCM with AEAD:** confidentiality and integrity are built in; this avoids CTR-only designs without authentication.
- **Airtime budgeting:** token-bucket enforcement with per-tier sub-budgets keeps usage predictable and regulation-aware.
- **3-tier reliability model:** Broadcast (fire-and-forget), Normal (acknowledged), and Critical (reliable with sliding-window flow control).
- **Cryptographic node identity:** X25519-derived 4-byte addresses provide stable, verifiable identity primitives.
- **Store-and-forward mailbox:** offline nodes receive queued messages when they rejoin the mesh.
- **Location sharing with privacy tiers:** presence, zone (coarse ~1km), or exact — per-peer control over what you share and with whom.
- **Emergency beacon:** dedicated priority channel for distress signaling.
- **Browser-based flashing:** flash firmware to new devices directly from the web — no toolchain required.

For a deeper feature-by-feature analysis, see [docs/COMPARISON.md](docs/COMPARISON.md).

## Web Client

Bramble includes a web client for live network operation and monitoring. It provides real-time chat (including delivery badges), map-based peer location views, neighbor and route visualization, traffic monitoring, channel management, and radio configuration.

![Chat](docs/images/webapp-chat.png)
![Nodes](docs/images/webapp-nodes.png)
![Map](docs/images/webapp-map.png)
![Stats](docs/images/webapp-stats.png)
![Config](docs/images/webapp-config.png)

See [docs/webapp/chat.md](docs/webapp/chat.md) for current web client behavior and usage notes.

## Hardware Targets

| Board | MCU | Display | Input | Radio | Audio | Status |
|------|-----|---------|-------|-------|-------|--------|
| Heltec WiFi LoRa 32 V3 | ESP32-S3 | 0.96" SSD1306 OLED (128x64) | Buttons | SX1262 | N/A | Running target |
| Heltec WiFi LoRa 32 V4 | ESP32-S3 | OLED + optional L76K GNSS | Buttons | SX1262 | N/A | Running target (active bring-up) |
| LilyGo T-Deck Plus | ESP32-S3 | ST7789 320x240 LCD with LVGL v9 UI | GT911 capacitive touch + I2C keyboard | SX1262 with TCXO (DIO3 1.8V, DC-DC) | I2S with NVS-persisted volume | Running target with full GUI |

## Simulator

Bramble ships with a mesh simulator that runs real protocol code against a virtual radio layer and renders topology/traffic in a browser. It is useful for repeatable scenario testing, failure injection, and behavior analysis before field deployment.

See [simulator/README.md](simulator/README.md) for setup and scenarios.

## Getting Started

Full build/flash instructions (including board-specific profiles and USB-port notes) are in [docs/BUILDING.md](docs/BUILDING.md).

Quick start:

```bash
cd ~/src/bramble
bash scripts/flash.sh local heltec-v3 build
bash scripts/flash.sh local heltec-v3 flash /dev/ttyUSB0
```

Use `tdeck-plus` instead of `heltec-v3` for T-Deck Plus builds.

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

- [docs/README.md](docs/README.md) — documentation index (recommended entry point)
- [docs/BUILDING.md](docs/BUILDING.md) — build, flash, monitor workflows
- [docs/bramble-architecture.md](docs/bramble-architecture.md) — component-level architecture
- [docs/COMPARISON.md](docs/COMPARISON.md) — comparison with other mesh systems
- [docs/bramble-protocol-spec.md](docs/bramble-protocol-spec.md) — protocol details
- [docs/bramble-testing.md](docs/bramble-testing.md) — test strategy and coverage
- [docs/ota-rollout.md](docs/ota-rollout.md) — OTA operator workflow
- [docs/quality-policy.md](docs/quality-policy.md) — repo-wide CI gates, promotion criteria, and rollback levers
- [docs/quality-policy-firmware.md](docs/quality-policy-firmware.md) — firmware lint/static-analysis phased rollout and advisory CI mapping
- [docs/quality-policy-webapp.md](docs/quality-policy-webapp.md) — webapp workflow required/advisory mapping, local parity commands, and rollback levers
- [simulator/README.md](simulator/README.md) — simulator usage
- [docs/webapp/chat.md](docs/webapp/chat.md) — web client chat and UX notes

## API and SDK

Bramble exposes a JSON-RPC 2.0 interface for device control and observability.

- [api/openapi.yaml](api/openapi.yaml) — OpenAPI source for the RPC surface
- [bramble-go](https://git.idiotica.org/dumbot/bramble-go) — Go SDK (serial, WebSocket, BLE)
- [bramble-cli](https://git.idiotica.org/dumbot/bramble-cli) — CLI/TUI built on bramble-go
- [VERSIONING.md](VERSIONING.md) — compatibility matrix

## CI/CD

- [.gitea/workflows/webapp-quality.yml](.gitea/workflows/webapp-quality.yml) runs webapp required gates (lint/typecheck/unit/build) plus an advisory e2e smoke lane.
- [.gitea/workflows/webapp-build-publish.yml](.gitea/workflows/webapp-build-publish.yml) builds/tests/publishes the web client image.
- [.gitea/workflows/firmware-quality.yml](.gitea/workflows/firmware-quality.yml) runs firmware quality gates (Phase 2.3): required clang-format/shellcheck/actionlint plus advisory clang-tidy/markdownlint.
- Additional workflow definitions live in [.gitea/workflows](.gitea/workflows).

## Status

Bramble is **pre-alpha**, but active and running on real hardware today (including T-Deck Plus, Heltec V3, and Heltec V4 bring-up). The protocol stack is implemented end-to-end, and host-side validation currently covers **59 test suites with 430+ individual test cases**. Development is ongoing.

## License

MIT — see [LICENSE](LICENSE)
