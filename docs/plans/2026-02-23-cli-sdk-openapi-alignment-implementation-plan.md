# CLI/SDK/OpenAPI Alignment + Go-Native Tailing Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Make Bramble CLI the protocol-faithful source of truth by aligning firmware RPC, OpenAPI, Go SDK, and CLI behavior, and add first-class Go-native tail/monitor capability so serial/python diagnostics are no longer required.

**Architecture:** Treat `api/openapi.yaml` as canonical contract, then enforce contract parity downward (firmware RPC response/request shapes) and upward (generated/updated Go SDK models and CLI command coverage). Add monitor/tail UX entirely in Go CLI using existing transport channels and event APIs, with optional firmware event refinements only where strictly needed.

**Tech Stack:** ESP-IDF firmware C, OpenAPI YAML, Go SDK (`bramble-go`), Go CLI (`bramble-cli`), Cobra commands, JSON-RPC.

## Status — ✅ COMPLETE (2026-02-24)

Verified complete against repo state + evidence:
- Firmware/API alignment landed (OpenAPI + RPC canonicalization + monitor/location notifications).
- Go SDK parity landed (`0.4.0` compatibility + location/monitor decoding updates).
- CLI parity landed (`location set-config/get-config`, protocol-native monitor topic filters).
- CLI-only 3-node validation evidence recorded in `docs/plans/evidence/2026-02-23-cli-only-location-validation.md`.

Key commits present across repos include:
- bramble: `c331ce3` (merge: location sharing + cli/sdk/openapi alignment), `84cc856`, `a995b2e`, `b35d32e`
- bramble-go: `cf4394c`, `5fe4f98`, `d1bafe7`
- bramble-cli: `0c52672`, `e4b3434`, `f348e32`, `6a5e6ee`

---

### Task 1: Lock canonical OpenAPI schemas for location + monitoring

**Files:**
- Modify: `api/openapi.yaml`
- Test: schema lint/validation command used in repo (or fallback parser check)

**Step 1: Write failing contract checks (golden schema assertions)**
- Add/adjust tests in SDK/CLI that currently fail due shape mismatch (`addr` typing, location config fields).

**Step 2: Define canonical types in OpenAPI**
- `LocationConfig` object (enabled, tier/default_tier, interval_s, source, contact_rules[], channel_targets[])
- `LocationPeer` object (`addr` as string hex, `name`, `tier`, `position`, `online`, `lastUpdatedMs`)
- `getPeerLocations` response (`peerLocations` canonical, `peers` optional alias if retained)
- `setLocationConfig`, `setLocationContact`, `removeLocationContact`, `shareLocationOnce` request/response schemas

**Step 3: Validate schema parses cleanly**
- Run repo schema validation command (or yaml/openapi parser check).

**Step 4: Commit**
```bash
git add api/openapi.yaml
git commit -m "spec(api): align location and monitor RPC schemas with protocol"
```

---

### Task 2: Align firmware RPC serialization to canonical schema

**Files:**
- Modify: `main/rpc_methods.c`
- Modify: `docs/api/rpc.md`
- Test: `test/test_rpc_methods.c`, `test/test_rpc_get_config_channel_export.c`

**Step 1: Add failing firmware contract tests**
- Assert `getPeerLocations.result.peerLocations[*].addr` is string hex.
- Assert `getConfig.result.location` includes canonical fields.

**Step 2: Implement strict schema output in firmware**
- Ensure canonical response fields always emitted with correct types.
- Keep compatibility aliases only if explicitly documented in OpenAPI.

**Step 3: Update rpc docs**
- Reflect exact response/request shapes from OpenAPI.

**Step 4: Verify tests**
```bash
cd test/build
cmake ..
make -j$(nproc) test_rpc_methods test_rpc_get_config_channel_export
./test_rpc_methods
./test_rpc_get_config_channel_export
```
Expected: PASS.

**Step 5: Commit**
```bash
git add main/rpc_methods.c docs/api/rpc.md test/test_rpc_methods.c test/test_rpc_get_config_channel_export.c
git commit -m "fix(rpc): enforce canonical location response/request schema"
```

---

### Task 3: Align Go SDK models + decoding to OpenAPI

**Files:**
- Modify: `/home/justin/src/bramble-go/types.go`
- Modify: `/home/justin/src/bramble-go/client.go`
- Modify/Add tests: `/home/justin/src/bramble-go/types_test.go`, `/home/justin/src/bramble-go/client_test.go`

**Step 1: Add failing SDK decode tests**
- Decode canonical `getPeerLocations` payload with string `addr`.
- Decode `getConfig.location` with full location policy fields.

**Step 2: Implement model updates**
- Make structs match OpenAPI exactly.
- Preserve backward decode only if OpenAPI compatibility section requires it.

**Step 3: Run SDK tests**
```bash
cd /home/justin/src/bramble-go
go test ./...
```
Expected: PASS.

**Step 4: Commit**
```bash
git -C /home/justin/src/bramble-go add types.go client.go types_test.go client_test.go
git -C /home/justin/src/bramble-go commit -m "feat(sdk): align location models/decoding with OpenAPI"
```

---

### Task 4: Add missing CLI location config parity commands

**Files:**
- Modify: `/home/justin/src/bramble-cli/internal/commands/location.go`
- Modify: `/home/justin/src/bramble-cli/internal/commands/config.go` (if needed for typed display)
- Add tests: `/home/justin/src/bramble-cli/internal/commands/location_test.go`

**Step 1: Write failing CLI tests**
- `location set-config` builds correct request payload.
- `location get-config` prints canonical location policy fields.

**Step 2: Implement commands**
- `bramble location set-config` flags:
  - `--enabled`, `--tier`, `--interval`, `--source`
  - repeatable contact/channel flags (or json-file input)
- `bramble location get-config`

**Step 3: Verify CLI tests + build**
```bash
cd /home/justin/src/bramble-cli
go test ./...
go build -o ./bin/bramble ./cmd/bramble
```
Expected: PASS.

**Step 4: Commit**
```bash
git -C /home/justin/src/bramble-cli add internal/commands/location.go internal/commands/config.go internal/commands/location_test.go
git -C /home/justin/src/bramble-cli commit -m "feat(cli): add location set-config/get-config protocol parity"
```

---

### Task 5: Implement Go-native tailing/monitor enhancements

**Files:**
- Modify: `/home/justin/src/bramble-cli/internal/commands/monitor.go`
- Modify: `/home/justin/src/bramble-go/client.go` (if streaming API extensions needed)
- Add tests: `/home/justin/src/bramble-cli/internal/commands/monitor_test.go`

**Step 1: Write failing monitor tests**
- Filter by topic (`--topic wifi,gps,mesh,location,traffic`).
- Follow behavior and grep behavior.

**Step 2: Implement monitor UX**
- `--follow` (default true)
- `--since <duration>`
- `--topic <csv>`
- `--grep <pattern>`
- `--json` structured event output

**Step 3: Verify tests + binary**
```bash
cd /home/justin/src/bramble-cli
go test ./...
go build -o ./bin/bramble ./cmd/bramble
```
Expected: PASS.

**Step 4: Commit**
```bash
git -C /home/justin/src/bramble-cli add internal/commands/monitor.go internal/commands/monitor_test.go
# plus any required sdk file(s)
git -C /home/justin/src/bramble-cli commit -m "feat(cli): add protocol-native monitor tail filters"
```

---

### Task 6: End-to-end CLI-only validation on 3-node hardware

**Files:**
- Create: `docs/plans/evidence/2026-02-23-cli-only-location-validation.md`
- Modify: `docs/testing/network-reach-e2e-checklist.md`

**Step 1: Build + deploy firmware and CLI binaries**
- Flash all 3 nodes with current branch firmware.
- Use updated CLI binary only.

**Step 2: Run CLI-only matrix**
- Set location config from CLI on T-Deck.
- Set contacts from CLI on T-Deck.
- Verify Heltecs show T-Deck location via `location status`.
- Disable sharing from CLI.
- Verify `lastUpdatedMs` stops changing for monitoring window.

**Step 3: Verify monitor tail command replaces ad-hoc serial probes**
- Demonstrate `bramble monitor --topic location,wifi --grep "TX location packet"` and equivalent useful outputs.

**Step 4: Record evidence**
- Include commands, outputs, pass/fail notes, and caveats.

**Step 5: Commit**
```bash
git add docs/plans/evidence/2026-02-23-cli-only-location-validation.md docs/testing/network-reach-e2e-checklist.md
git commit -m "chore(verify): add CLI-only location and monitor validation evidence"
```

---

### Task 7: Versioning + release notes for alignment changes

**Files:**
- Modify: `main/rpc_methods.c` and/or canonical version source if protocol change requires bump
- Modify: `CHANGELOG.md` (or equivalent release notes file)
- Modify: `/home/justin/src/bramble-go/version.go`
- Modify: `/home/justin/src/bramble-cli/README.md`

**Step 1: Determine if protocol bump is required**
- If schema/type changes are breaking, bump protocol semver accordingly.

**Step 2: Update SDK/CLI version metadata**
- Ensure compatibility ranges align with firmware protocol.

**Step 3: Document changes for operators**
- Explicitly list CLI command additions and monitor tail usage.

**Step 4: Commit**
```bash
git add <all version/release-note files>
git commit -m "chore(release): align protocol/sdk/cli versions and notes"
```

---

## Definition of Done

- OpenAPI is canonical and matches firmware RPC payloads exactly.
- Go SDK decodes/encodes canonical payloads without custom hacks.
- CLI has full location policy parity (`set-config`, `get-config`) and is protocol-complete for location workflows.
- CLI monitor/tail command can replace ad-hoc serial/python diagnostics for routine ops.
- 3-node CLI-only validation demonstrates:
  - location sends when enabled,
  - location stops when disabled,
  - receiver status output is correct and stable.
- Evidence doc exists with exact command/output proof.
