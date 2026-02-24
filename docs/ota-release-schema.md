# OTA Release Index Schema

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
          "file": "/ota/stable/v0.4.0/heltec-v3/bramble-heltec.bin",
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
- `notes` optional

## Ordering
Consumers must display newest first, sorted by:
1. `published_at` descending
2. semantic version descending (tie-breaker)
