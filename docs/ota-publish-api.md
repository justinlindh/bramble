# OTA Publish API (`POST /ota/publish`)

This endpoint is used by CI to publish OTA artifacts and trigger `index.json` regeneration.

## Authentication

All requests **must** include:

```http
Authorization: Bearer OTA_PUBLISH_KEY
```

- Missing or invalid bearer token returns `401 Unauthorized`.
- `OTA_PUBLISH_KEY` is configured server-side via environment variable.

## Endpoint

- **Method:** `POST`
- **Path:** `/ota/publish`
- **Content-Type:** `multipart/form-data`

## Request Fields

### Required form fields
- `version` — release version tag (semver-style string, e.g. `v0.4.0`)
- `channel` — `stable` or `dev`
- `board` — target board id, e.g. `heltec-v3`
- file `bootloader.bin`
- file `partition-table.bin`
- file `bramble.bin`

### Optional form fields
- `commit` — source commit SHA
- `run_id` — CI run identifier
- `published_at` — ISO-8601 UTC timestamp override (otherwise server-generated)

### Test fixture note (important)
The JSON fixtures under `test/fixtures/ota-publish-request-*.json` represent the **normalized server-side request shape after multipart parsing**, not the raw HTTP wire format.

Normalized fixture shape:
- `form.version`, `form.channel`, `form.board`, optional metadata
- `files.<filename>` entries mapping required upload fields

## Successful Response (`200`)

```json
{
  "ok": true,
  "release": {
    "version": "v0.4.0",
    "channel": "stable",
    "board": "heltec-v3",
    "published_at": "2026-02-24T18:00:00Z",
    "artifacts": [
      {
        "name": "bootloader.bin",
        "canonical_file": "/ota/stable/v0.4.0/heltec-v3/bootloader.bin",
        "tagged_file": "/ota/stable/v0.4.0/heltec-v3/bootloader-v0.4.0.bin",
        "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "size": 32768
      },
      {
        "name": "partition-table.bin",
        "canonical_file": "/ota/stable/v0.4.0/heltec-v3/partition-table.bin",
        "tagged_file": "/ota/stable/v0.4.0/heltec-v3/partition-table-v0.4.0.bin",
        "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "size": 4096
      },
      {
        "name": "bramble.bin",
        "canonical_file": "/ota/stable/v0.4.0/heltec-v3/bramble.bin",
        "tagged_file": "/ota/stable/v0.4.0/heltec-v3/bramble-v0.4.0.bin",
        "sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "size": 1048576
      }
    ],
    "commit": "5f6c8e8a5f4d3f3f9a0a2fdd6f2b8c3d7f3e9abc",
    "run_id": "gitea-123456"
  },
  "indexPath": "/ota/index.json"
}
```

## Mapping to `index.json` schema

The publish response includes per-file detail (`name`, `canonical_file`, `tagged_file`) for immediate CI visibility.

Persisted `index.json` entries use the release schema in `docs/ota-release-schema.md`:
- `board` comes from request form field
- `file` maps to `canonical_file` only
- `sha256` and `size` are copied from computed publish results

Each board release contributes **three canonical artifact entries** in `index.json`:
1. `bootloader.bin`
2. `partition-table.bin`
3. `bramble.bin`

Semver-tagged copies are published and retained on disk, but are not the primary `file` targets for flasher consumers.

## Error Responses

All non-2xx responses follow this shape:

```json
{
  "ok": false,
  "error": {
    "code": "BAD_REQUEST",
    "message": "human-readable message",
    "details": {
      "field": "version"
    }
  }
}
```

### `400 Bad Request`
Validation or malformed payload.

```json
{
  "ok": false,
  "error": {
    "code": "BAD_REQUEST",
    "message": "Missing required file: bramble.bin",
    "details": {
      "missing": ["bramble.bin"]
    }
  }
}
```

### `401 Unauthorized`
Missing/invalid bearer token.

```json
{
  "ok": false,
  "error": {
    "code": "UNAUTHORIZED",
    "message": "Invalid bearer token"
  }
}
```

### `409 Conflict`
Release already published for `(channel, version, board)` or idempotency collision.

```json
{
  "ok": false,
  "error": {
    "code": "CONFLICT",
    "message": "Release already exists for stable/v0.4.0/heltec-v3",
    "details": {
      "channel": "stable",
      "version": "v0.4.0",
      "board": "heltec-v3"
    }
  }
}
```

### `500 Internal Server Error`
Unexpected server-side failure.

```json
{
  "ok": false,
  "error": {
    "code": "INTERNAL_ERROR",
    "message": "Failed to rebuild OTA index"
  }
}
```

## Example cURL Request

```bash
curl -X POST "https://bramblemesh.org/ota/publish" \
  -H "Authorization: Bearer $OTA_PUBLISH_KEY" \
  -F "version=v0.4.0" \
  -F "channel=stable" \
  -F "board=heltec-v3" \
  -F "commit=$GITEA_SHA" \
  -F "run_id=$GITEA_RUN_ID" \
  -F "bootloader.bin=@build/bootloader.bin" \
  -F "partition-table.bin=@build/partition-table.bin" \
  -F "bramble.bin=@build/bramble.bin"
```
