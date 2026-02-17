# Bramble

A privacy-first LoRa mesh networking protocol for ESP32, built with ESP-IDF.

Bramble enables encrypted, multi-hop communication between low-power LoRa devices without relying on any central infrastructure. Every packet is encrypted end-to-end, routing metadata is protected, and the protocol is designed to resist traffic analysis.

## Features

- **End-to-end encryption** — AES-256-GCM with X25519 key exchange
- **Multi-hop mesh routing** — AODV-inspired with encrypted route requests and salt rotation
- **Privacy by design** — Encrypted RREQ source addresses, optional dummy traffic mode
- **3-tier reliability** — Fire-and-forget, acknowledged, and reliable delivery modes
- **Airtime budgeting** — Per-node duty cycle enforcement for regulatory compliance
- **Fragmentation** — Large messages split and reassembled transparently
- **Time synchronization** — Lightweight mesh-wide clock sync
- **Key backup** — Physical button authentication for key recovery
- **Channel support** — Named encrypted group channels with flood-based delivery
- **OTA updates** — Over-the-air firmware updates via mesh

## Architecture

Bramble is organized as ESP-IDF components, each self-contained with clean interfaces:

| Component | Purpose |
|-----------|---------|
| `crypto` | AES-256-GCM, X25519, key derivation, anti-replay |
| `routing` | AODV route discovery, forwarding tables, route maintenance |
| `security` | Session management, key exchange, identity verification |
| `packet` | Packet framing, serialization, type definitions |
| `reliability` | ACKs, retransmission, delivery confirmation |
| `fragment` | Message fragmentation and reassembly |
| `channel` | Named group channels with flood routing |
| `radio` | SX1262 LoRa driver abstraction |
| `airtime` | Duty cycle tracking and TX budgeting |
| `dedup` | Duplicate packet detection |
| `identity` | Node identity and address management |
| `timesync` | Mesh time synchronization |
| `display` | OLED status display |
| `ble` | BLE interface for mobile companion app |
| `ota` | Over-the-air firmware updates |
| `ui` | JSON-RPC interface for external control |

## Hardware Targets

- **Heltec WiFi LoRa 32 V3** (ESP32-S3 + SX1262) — primary target
- **LILYGO T-Beam Supreme** (ESP32-S3 + SX1262) — secondary target

## Building

Requires [ESP-IDF v5.4](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/get-started/).

```bash
. ~/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

## Testing

29 test suites covering crypto, routing, security, integration, and more. Tests use the [Unity](https://github.com/ThrowTheSwitch/Unity) framework and run on the host (no hardware needed):

```bash
cd test
./run_all_tests.sh
```

## Simulator

A full mesh network simulator with real-time web visualization. Runs actual Bramble C code against a virtual radio layer with configurable topology, packet loss, interference zones, and anomaly detection.

```bash
cd simulator
docker compose up --build
```

Open http://localhost:3003 to see the mesh in action. Drag nodes to reposition them, add/remove nodes on the fly, control playback speed, and load different scenarios.

See [`simulator/README.md`](simulator/README.md) for details.

## Documentation

- [`docs/plans/2026-02-15-lora-mesh-protocol-design.md`](docs/plans/2026-02-15-lora-mesh-protocol-design.md) — Full protocol design (~2400 lines)
- [`docs/bramble-anomaly-detection.md`](docs/bramble-anomaly-detection.md) — Anomaly detection system
- [`docs/COMPARISON.md`](docs/COMPARISON.md) — Comparison with Meshtastic and other protocols
- [`docs/field-test-5node.md`](docs/field-test-5node.md) — 5-node field test plan
- [`docs/field-test-20node.md`](docs/field-test-20node.md) — 20-node field test plan

## Status

The protocol layer is implemented and tested in software. Hardware-dependent work (SX1262 SPI driver, FreeRTOS tasks, OLED/BLE/OTA) is paused pending hardware availability.

## License

Private — not yet licensed for distribution.
