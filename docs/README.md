# Bramble Documentation Index

This index separates **operator/developer reference docs** from **historical planning artifacts**.

## Start here

- [getting-started.md](getting-started.md): first-node walkthrough from flashing to your first confirmed message (provisioning, unprovisioned banner, optional trust anchor)
- [../README.md](../README.md): project overview and quick start
- [BUILDING.md](BUILDING.md): build/flash/monitor workflows (board-aware)
- [../CONTRIBUTING.md](../CONTRIBUTING.md): contributor setup, quality gates, branch and commit conventions
- [updating-your-node.md](updating-your-node.md): update a node over the air from the web client (channels, signing trust, downgrades, recovery)
- [troubleshooting.md](troubleshooting.md): common build, flash, and test stalls (serial permissions, toolchain targets, port collisions)
- [bramble-testing.md](bramble-testing.md): test matrix and what to run
- [bramble-architecture.md](bramble-architecture.md): component architecture
- [bramble-protocol-spec.md](bramble-protocol-spec.md): protocol behavior and packet semantics
- [SECURITY-MODEL.md](SECURITY-MODEL.md): threat model, verified protections, and known gaps
- [auth.md](auth.md): RPC authentication, pairing, and the browser origin allowlist
- [network-key-provisioning.md](network-key-provisioning.md): generating, distributing, and verifying the control-plane network key across a fleet
- [trust-anchor.md](trust-anchor.md): optional per-fleet trust anchor: enrollment ceremony, endorsed-only pinning, and what Sybil scarcity it does and does not close
- [api/rpc.md](api/rpc.md): RPC method reference and wire-format notes
- [digital-twin.md](digital-twin.md): import a deployment's observed topology into the simulator and ask capacity and node-loss questions about that mesh
- [COMPARISON.md](COMPARISON.md): comparison with Meshtastic and MeshCore
- [results/simulation-2026-07-honest-baseline.md](results/simulation-2026-07-honest-baseline.md): current measured scale results (supersedes [results/simulation-2026-06.md](results/simulation-2026-06.md))

## Hardware & board bring-up

- [device-screens.md](device-screens.md): gallery of the primary screens on all three display classes (T-Deck Plus LVGL, Heltec OLED, Pager e-paper), captured from real hardware and the emulator
- Board pin maps live in code: `main/boards/*.h` (source of truth)
- [BUILDING.md](BUILDING.md): board-aware build/flash/monitor
- [../hardware/pager/v1/](../hardware/pager/v1/): Bramble Pager v1 custom PCB design tree (spec, schematic, PCB, BOM, enclosure, errata)

## Operations / runbooks

- [updating-your-node.md](updating-your-node.md): the web-client firmware-update journey (start here for a single node)
- [ota-rollout.md](ota-rollout.md): OTA operator runbook: RPC recipes, local dev-build origin, downgrades, and USB recovery
- [design/ota-signing.md](design/ota-signing.md): signed OTA trust model, key infrastructure, and rotation
- [design/ota-antirollback.md](design/ota-antirollback.md): eFuse anti-rollback design and its pending bench gate
- [ota-release-schema.md](ota-release-schema.md): `index.json` release-index schema and artifact path layout
- [ci.md](ci.md): why the PR-gating workflows always trigger and skip per job instead of using workflow-level path filters, the context naming contract, and each job's skip condition

## Webapp docs

- [webapp/chat.md](webapp/chat.md)
- [webapp/desktop.md](webapp/desktop.md): Electron desktop app (install, connect, Nearby nodes)
- [../webapp/README.md](../webapp/README.md): webapp dev/build workflow and packaging
