# OTA Release Index Schema

Last verified: 2026-03-01

`index.json` shape:

```json
{
  "releases": [
    {
      "version": "v0.4.0",
      "published_at": "2026-02-24T08:00:00Z",
      "channel": "stable",
      "artifacts": [
        {
          "board": "heltec-v3",
          "file": "/ota/stable/v0.4.0/heltec-v3/bramble.bin",
          "sha256": "<64 hex>",
          "size": 1048576,
          "notes": "optional"
        }
      ]
    }
  ]
}
```

## Required per release
- `version` (string)
- `published_at` (ISO-8601 UTC)
- `channel` (`stable` or `dev`)
- `artifacts[]`

## Required per artifact
- `board`
- `file`
- `sha256` (64 hex chars)
- `size` (positive integer)

## Optional per artifact
- `notes`

## Filename policy (canonical + semver-tagged)
For each uploaded artifact, publisher writes both:

1. **Canonical filename** (stable path for flasher/runtime compatibility)
   - `bootloader.bin`
   - `partition-table.bin`
   - `bramble.bin`

2. **Semver-tagged filename** (immutable release traceability)
   - `bootloader-<version-without-leading-v>.bin`
   - `partition-table-<version-without-leading-v>.bin`
   - `bramble-<version-without-leading-v>.bin`

Under release directory:
- `/ota/<channel>/<version>/<board>/`

Example:
- `/ota/stable/v0.4.0/heltec-v3/bramble.bin` (canonical)
- `/ota/stable/v0.4.0/heltec-v3/bramble-0.4.0.bin` (tagged copy)

`index.json` should continue to reference canonical files for consumer stability; tagged copies are retained for provenance/debugging and may be required by tooling checks.

## Ordering
Consumers must display newest first, sorted by:
1. `published_at` descending
2. semantic version descending (tie-breaker)
