# Gitea Runner Baseline (2026-02-24)

## Current platform
- Host: GPU box `192.0.2.199`
- Gitea container: `gitea` (`gitea/gitea:latest`)
- Gitea version: `1.25.4`
- Gitea DB: `gitea-db` (postgres)

## Actions/Runner status
- No `act_runner` service/container currently running.
- No system service runner found (`act_runner` not present as systemd unit).
- Repository workflows currently include webapp CI; no containerized firmware build workflow yet.

## Docker capabilities
- Docker available on host.
- Existing stack already uses docker socket mounts for other automation (`webhook-listener`).

## Known immediate requirement
- Register and run a dedicated Gitea Actions runner with persistent config/cache.

## Rollback (pre-runner)
No rollback needed yet; runner not deployed at this baseline.
