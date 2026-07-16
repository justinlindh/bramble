# Bramble Documentation Index

This index separates **operator/developer reference docs** from **historical planning artifacts**.

## Start here

- [getting-started.md](getting-started.md): first-node walkthrough from flashing to your first confirmed message (provisioning, unprovisioned banner, optional trust anchor)
- [../README.md](../README.md): project overview and quick start
- [BUILDING.md](BUILDING.md): build/flash/monitor workflows (board-aware)
- [bramble-testing.md](bramble-testing.md): test matrix and what to run
- [bramble-architecture.md](bramble-architecture.md): component architecture
- [bramble-protocol-spec.md](bramble-protocol-spec.md): protocol behavior and packet semantics
- [SECURITY-MODEL.md](SECURITY-MODEL.md): threat model, verified protections, and known gaps
- [auth.md](auth.md): RPC authentication, pairing, and the browser origin allowlist
- [network-key-provisioning.md](network-key-provisioning.md): generating, distributing, and verifying the control-plane network key across a fleet
- [trust-anchor.md](trust-anchor.md): optional per-fleet trust anchor: enrollment ceremony, endorsed-only pinning, and what Sybil scarcity it does and does not close
- [api/rpc.md](api/rpc.md): RPC method reference and wire-format notes
- [COMPARISON.md](COMPARISON.md): comparison with Meshtastic and MeshCore
- [results/simulation-2026-07-honest-baseline.md](results/simulation-2026-07-honest-baseline.md): current measured scale results (supersedes [results/simulation-2026-06.md](results/simulation-2026-06.md))

## Hardware & board bring-up

- Board pin maps live in code: `main/boards/*.h` (source of truth)
- [BUILDING.md](BUILDING.md): board-aware build/flash/monitor
- [../hardware/pager/v1/](../hardware/pager/v1/): Bramble Pager v1 custom PCB design tree (spec, schematic, PCB, BOM, enclosure, errata)
- Historical bring-up notes (pinmap research, GNSS bring-up, display debugging) are in [archive/](archive/)

## Operations / runbooks

- [ota-rollout.md](ota-rollout.md)
- [design/ota-signing.md](design/ota-signing.md): signed OTA trust model, key infrastructure, and rotation
- [runbooks/ota-publish-endpoint-runbook.md](runbooks/ota-publish-endpoint-runbook.md)
- [ci.md](ci.md): why the PR-gating workflows always trigger and skip per job instead of using workflow-level path filters, the context naming contract, and each job's skip condition

## Webapp docs

- [webapp/chat.md](webapp/chat.md)
- [webapp/desktop.md](webapp/desktop.md): Electron desktop app (install, connect, Nearby nodes)
- [../webapp/README.md](../webapp/README.md): webapp dev/build workflow and packaging

## Testing checklists

- [testing/network-reach-e2e-checklist.md](testing/network-reach-e2e-checklist.md)

## Historical plans and evidence (non-authoritative)

- [archive/README.md](archive/README.md): archive policy and historical index
- [archive/plans/](archive/plans/): implementation plans, investigations, and evidence snapshots

> `docs/archive/plans/**` is retained as project history and working notes. Treat it as archival context, not normative operator guidance.
