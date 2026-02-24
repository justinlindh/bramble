# Docker/Build Cache Strategy for Gitea Runner

## Objectives
- Avoid re-pulling/rebuilding full firmware toolchains each run.
- Keep cache persistent across runner restarts.
- Preserve deterministic builds while allowing controlled cache busting.

## Cache layers
1. **Runner action cache**
   - Backing path: runner `/data/cache`
   - Purpose: `actions/cache` tarball caches (npm, pip, etc.)

2. **BuildKit layer cache**
   - Backing path: `/var/lib/buildkit-cache` (runner volume)
   - Purpose: Docker buildx layer reuse for firmware builder image

3. **Docker image cache (host daemon)**
   - Reuse pulled image layers from host Docker daemon

## Current volume mapping
- `dockers_gitea-runner-data` -> `/data`
- `dockers_gitea-runner-cache` -> `/cache`
- `dockers_gitea-runner-buildkit` -> `/var/lib/buildkit-cache`

## Workflow conventions
- Use branch-aware cache keys: `<job>-<branch>-<lockfile-hash>`
- Add restore keys for fallback to default branch
- For firmware builder docker builds, use explicit buildx local cache dirs

## Cache busting
Bust cache when any of these change:
- ESP-IDF version
- toolchain base image tag
- Dockerfile major dependency set

Recommended key suffix: `idf-<version>-toolchain-<tag>`

## Maintenance
- If cache corruption suspected:
  - stop runner
  - remove only affected volume (`gitea-runner-buildkit` or cache)
  - restart runner
- Periodic pruning can be done during maintenance windows.
