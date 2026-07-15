# Versioning

## Repository Structure

```
bramble (this repo)          bramble-go              bramble-cli
├── main/                    ├── transport/           ├── cmd/
├── components/              ├── client/              └── main.go
├── api/openapi.yaml         └── go.mod
├── simulator/
└── webapp/
        │                         │                        │
        │   ┌─────────────────────┘                        │
        │   │ consumes api/openapi.yaml                    │
        │   │                         ┌────────────────────┘
        │   │                         │ depends on bramble-go
        ▼   ▼                         ▼
   ┌──────────────────────────────────────────┐
   │  Bramble device (firmware + JSON-RPC)    │
   └──────────────────────────────────────────┘
```

| Repo | Contains |
|------|----------|
| **bramble** | ESP32 firmware, OpenAPI spec (`api/openapi.yaml`), simulator, webapp |
| **bramble-go** | Go SDK: transport layer (BLE/serial), JSON-RPC client |
| **bramble-cli** | CLI tool built on bramble-go |

## Protocol Version

The **protocol version** defines the JSON-RPC API contract between firmware and clients.

- **Spec:** `api/openapi.yaml` (`info.version`). The spec is synced to the firmware's RPC registry (`main/rpc_methods.c`); the Quality CI workflow runs `scripts/check-rpc-contract.sh`, which fails the build on any method-name drift between the two.
- **Runtime:** firmware returns it via `bramble.getVersion` → `protocol_version`
- **Independent** from firmware version; they evolve separately

**As of 2026-07-15:** the method surface is synced and CI-enforced, but the version strings are not aligned and the drift is expected to persist for a while. The spec declares `0.6.0` (`api/openapi.yaml` `info.version`) while the firmware constant reports `0.5.0` (`BRAMBLE_PROTOCOL_VERSION` in `main/rpc_methods.c`). The firmware string cannot be bumped to `0.6.0` on its own: `bramble-go` (the Go SDK) hardcodes `MaxProtocolVersion = "0.5.0"` (`version.go`), and any bramble-go client would refuse a `0.6.0` firmware as out of range. Bumping the firmware string is therefore blocked on a coordinated bramble-go release that raises the ceiling first, not just a firmware-side edit. The contract check (`scripts/check-rpc-contract.sh`) enforces method names between spec and firmware; it does not touch the version string.

Wire versions (the mesh packet format, currently at wire version 4, `BRAMBLE_VERSION`) are a separate, orthogonal counter from the protocol version string above; a wire-version bump changes packet layout on the air and does not require a protocol-version bump, and vice versa.

### Semver Rules

| Bump | When | Example |
|------|------|---------|
| **Patch** (0.1.x) | Bug fixes in method behavior, no schema changes | Fix edge case in `bramble.getVersion` response |
| **Minor** (0.x.0) | New methods added, existing methods unchanged | Add `bramble.setConfig` |
| **Major** (x.0.0) | Breaking changes to existing method params/responses | Rename a required field |

## Firmware Versioning

Independent semver for the firmware binary. Tracks hardware support, performance, and bug fixes. A firmware release may or may not change the protocol version.

## SDK Versioning (bramble-go)

Independent semver. Each release documents its supported protocol version range (min/max).

| Bump | When |
|------|------|
| **Patch** | Bug fixes, no API changes |
| **Minor** | Support for new protocol methods |
| **Major** | Breaking changes to Go client API |

## CLI Versioning (bramble-cli)

Independent semver. Each release documents which bramble-go version it depends on.

## Version Negotiation

1. SDK calls `bramble.getVersion` on connect
2. Firmware returns `protocol_version`
3. SDK checks if the version falls within its supported range
4. If incompatible → error: `"firmware protocol version X.Y.Z not supported (need >= A.B.C, < D.0.0)"`

## Compatibility Matrix

| bramble-go | Protocol Version | Firmware (min) |
|------------|------------------|----------------|
| v0.1.0     | 0.1.0            | v0.1.0         |
| v0.2.0     | 0.1.0-0.2.0      | v0.1.0         |
| v0.2.1     | 0.1.0-0.2.1      | v0.2.0-dev     |
| v0.4.2     | 0.1.0-0.5.0      | v0.4.1-dev     |
| v0.5.0     | 0.1.0-0.5.0      | v0.5.0-dev     |
| v0.12.0    | 0.1.0-0.5.0      | firmware `protocol_version 0.5.0` (current; no firmware release tags exist yet, see Firmware Versioning below) |

`bramble-go` has released through v0.12.0 (adding trust-anchor client methods and API polish) without moving `MaxProtocolVersion` past `0.5.0`; the spec's `0.6.0` is not reachable by any released SDK today.

## Adding a New RPC Method

1. Implement handler in firmware (`main/rpc_methods.c`)
2. Update `api/openapi.yaml`: add method schema
3. Bump protocol version (minor)
4. Implement in bramble-go client
5. Bump bramble-go version (minor)
6. Add CLI command in bramble-cli
7. Bump bramble-cli version (minor)
8. Update compatibility matrix above

## Component Release Commit Scopes (Phase 2)

Component semantic-release jobs are scope-gated. To trigger a component release, commit messages must use a Conventional Commit scope for that component:

- `protocol` → protocol release stream (`protocol-vX.Y.Z`)
- `webapp` → webapp release stream (`webapp-vX.Y.Z`)

Examples:

- `feat(protocol): add bramble.setConfig RPC method`
- `fix(webapp): handle reconnect after BLE adapter reset`

Commits without one of the above scopes do **not** trigger protocol/webapp component releases.

Planned (not yet enabled in workflow):

- `firmware` → `firmware-vX.Y.Z`
- `sim` → `sim-vX.Y.Z`

## Breaking Changes

A **breaking change** is any modification to an existing method that could cause working clients to fail:

- Removing a method
- Changing or removing required params
- Changing response field names or types
- Changing error codes for existing conditions

### Deprecation Process

1. Mark the method/field `deprecated` in `api/openapi.yaml` with a description noting the replacement
2. Release at least one minor version with the deprecation notice
3. Remove in the next major version

### Migration Guide Template

When bumping a major protocol version, add a section to this file:

```markdown
### Migrating from Protocol vX to vY

| Changed | Before | After | Action |
|---------|--------|-------|--------|
| `method.name` | old behavior | new behavior | update call to ... |
```
