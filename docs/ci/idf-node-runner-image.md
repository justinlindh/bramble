# idf-node runner image provenance (pre-baked ESP-IDF)

This document records the authoritative source and deployment pin for the `idf-node` execution environment used by Bramble firmware CI jobs.

## Authoritative image source

- Repository: `bramble`
- Dockerfile: `docker/firmware-builder/Dockerfile`
- Base image pin: `espressif/idf:v5.4.1`
- Published runtime tag for runners: `bramble/idf-node:v5.4.1`

## Pre-baked contents required by firmware CI

The image pre-installs:

- ESP-IDF + Xtensa/RISC-V toolchains via `espressif/idf:v5.4.1`
- `uv` / `uvx` (pinned via `UV_VERSION` build arg)
- CI baseline tools used in workflows/scripts (`bash`, `curl`, `git`, `jq`, `make`, `rsync`, `unzip`, CA certificates)

## Runner fleet pin (deployment mapping)

Runner label mapping should pin `idf-node` to this image tag:

- `idf-node:docker://bramble/idf-node:v5.4.1`

The runner label mapping lives in the self-hosted runner's configuration on
whichever host runs the CI runner (GitHub Actions is the authoritative CI;
see [../ci.md](../ci.md)).

> If the pin changes, update this doc and runner config together, then re-verify firmware workflows.

## Build/publish/deploy flow

1. Build and publish updated image from this repository's Dockerfile.
2. Update runner label mapping (if tag changed) in runner config.
3. Restart/reload runner containers.
4. Trigger `firmware-build.yml` (or `firmware-quality.yml`) and verify jobs execute on `idf-node` with no runtime ESP-IDF bootstrap.
