# OTA Publish Endpoint Security

Last verified: 2026-03-01

## Endpoint exposure
- Publish endpoint is `POST /ota/publish`.
- Endpoint is intentionally unlinked from public UI and intended for CI automation only.
- Access is protected by `Authorization: Bearer <OTA_PUBLISH_KEY>`.

## Request controls
- Multipart limits enforced server-side:
  - max files: 10
  - max fields: 20
  - max file size: 25 MiB
- Request timeout enforced by publisher (`REQUEST_TIMEOUT_MS`, default `30000`).
- Nginx enforces request body ceiling (`client_max_body_size 32m`).

## Replay / abuse guards
- Optional `x-idempotency-key` header supported.
  - Format: `^[A-Za-z0-9._:-]{8,128}$`
  - Reuse of a previously seen key returns `409 DUPLICATE_REQUEST`.
- Optional `published_at` must be near server time.
  - Max skew controlled by `MAX_PUBLISHED_AT_SKEW_MS` (default 15 minutes).
  - Out-of-window requests are rejected.

## Audit logging
Publisher logs include:
- success: channel/version/board/run_id/commit
- failure: status/error code + channel/version/board/run_id + message
- unauthorized attempts are logged

## Operational guidance
- Rotate `OTA_PUBLISH_KEY` periodically and after runner credential exposure.
- Keep key only in CI secret store; never commit to repo.
- Restrict source IP/CIDR at reverse proxy/firewall layer if CI egress is stable.
- Monitor logs for repeated 401/409/413 responses.
