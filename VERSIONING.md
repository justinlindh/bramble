# Versioning

## Repository Structure

```
bramble (this repo)          bramble-go              bramble-cli
├── firmware/                ├── transport/           ├── cmd/
├── api/openapi.yaml         ├── client/              └── main.go
├── simulator/               └── go.mod
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
| **bramble-go** | Go SDK — transport layer (BLE/serial), JSON-RPC client |
| **bramble-cli** | CLI tool built on bramble-go |

## Protocol Version

The **protocol version** defines the JSON-RPC API contract between firmware and clients.

- **Source of truth:** `api/openapi.yaml` → `info.version`
- **Runtime:** firmware returns it via `bramble.getVersion` → `protocol_version`
- **Independent** from firmware version — they evolve separately

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
| v0.2.0     | 0.1.0            | v0.1.0         |

## Adding a New RPC Method

1. Implement handler in firmware (`components/rpc/rpc_methods.c`)
2. Update `api/openapi.yaml` — add method schema
3. Bump protocol version (minor)
4. Implement in bramble-go client
5. Bump bramble-go version (minor)
6. Add CLI command in bramble-cli
7. Bump bramble-cli version (minor)
8. Update compatibility matrix above

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
