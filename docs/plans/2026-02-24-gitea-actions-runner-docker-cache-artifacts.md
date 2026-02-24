# Gitea Actions Runner + Docker Cache + Artifacts Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Enable reliable Gitea CI for Bramble firmware with Docker-based builds, persistent caching, and working artifact upload/download.

**Architecture:** Add a dedicated self-hosted Gitea Actions runner service on the GPU box (dockerized), grant controlled Docker build capability, persist cache volumes for action cache + build layers, and validate with a smoke workflow before wiring firmware pipelines.

**Tech Stack:** Gitea 1.25.x, Gitea act_runner, Docker/Buildx, docker-compose in `~/src/dockers`, Gitea Actions workflows.

---

### Task 1: Baseline and safety guardrails

**Files:**
- Create: `docs/ci/gitea-runner-baseline.md`
- Create: `docs/ci/gitea-runner-threat-model.md`

**Step 1: Capture baseline state**
- Record current Gitea version, current services, and absence/presence of runner.

**Step 2: Define trust boundaries**
- Document implications of mounting `/var/run/docker.sock`.
- Restrict runner scope to target repos/org only.

**Step 3: Define rollback**
- Runner stop/remove steps.
- Cache volume cleanup steps.

**Step 4: Commit**
- `git add docs/ci/gitea-runner-baseline.md docs/ci/gitea-runner-threat-model.md`
- `git commit -m "docs(ci): baseline + runner security boundaries"`

---

### Task 2: Add Gitea Actions runner service (docker)

**Files:**
- Modify: `/home/justin/src/dockers/docker-compose.yml`
- Create: `/home/justin/src/dockers/gitea-runner/config.yaml`
- Create: `/home/justin/src/dockers/gitea-runner/.env.example`

**Step 1: Add runner container service**
- Use official Gitea `act_runner` image.
- Mount persistent config/data volume.
- Mount Docker socket (if required by selected strategy).

**Step 2: Configure registration flow**
- Use registration token from Gitea UI/API.
- Set labels/capabilities (e.g., `linux`, `docker`, `gpu-box`).

**Step 3: Start and verify runner visibility**
- `docker compose up -d <runner-service>`
- Confirm runner appears online in Gitea Admin/Repo Actions runners.

**Step 4: Commit**
- `git add` compose + runner config files.
- `git commit -m "feat(ci): add dockerized Gitea actions runner"`

---

### Task 3: Implement persistent Docker/build cache strategy

**Files:**
- Modify: runner config (`config.yaml`) for cache mounts
- Create: `docs/ci/docker-cache-strategy.md`

**Step 1: Add cache volumes**
- Persistent action cache location.
- Persistent Buildx local cache path (e.g., `/var/lib/buildkit-cache`).

**Step 2: Add workflow-level cache conventions**
- Standard key patterns per branch + lockfiles/toolchain tag.
- Cache busting strategy for ESP-IDF changes.

**Step 3: Add retention policy**
- Size/age guardrails and cleanup procedure.

**Step 4: Verify cache reuse**
- Run same test workflow twice and confirm reduced pull/build time on second run.

**Step 5: Commit**
- `git add docs/ci/docker-cache-strategy.md /home/justin/src/dockers/gitea-runner/config.yaml`
- `git commit -m "feat(ci): persist docker/build caches for actions runner"`

---

### Task 4: Validate artifacts pipeline with smoke workflow

**Files:**
- Create: `.gitea/workflows/ci-smoke-artifacts.yml`
- Create: `ci/smoke/README.md`

**Step 1: Add minimal workflow**
- Checks out repo.
- Generates small test artifact file.
- Uploads artifact.

**Step 2: Verify in Gitea UI**
- Confirm run success and artifact appears/downloads.

**Step 3: Add failure diagnostics**
- Capture runner logs + workflow logs for troubleshooting.

**Step 4: Commit**
- `git add .gitea/workflows/ci-smoke-artifacts.yml ci/smoke/README.md`
- `git commit -m "test(ci): add smoke workflow for runner + artifacts"`

---

### Task 5: Add containerized firmware build workflow skeleton

**Files:**
- Create: `docker/firmware-builder/Dockerfile`
- Create: `scripts/ci-build-firmware.sh`
- Create: `.gitea/workflows/firmware-build.yml`
- Modify: `docs/BUILDING.md`

**Step 1: Build pinned firmware builder image**
- Pin ESP-IDF/toolchain versions.

**Step 2: Add CI build script**
- Build target boards and output bins to artifact directory.

**Step 3: Wire workflow**
- Build image (or pull pinned tag), run builds, upload firmware artifacts.

**Step 4: Verify reproducibility**
- Two runs produce expected artifacts and stable metadata.

**Step 5: Commit**
- `git add docker/firmware-builder scripts/ci-build-firmware.sh .gitea/workflows/firmware-build.yml docs/BUILDING.md`
- `git commit -m "feat(ci): add containerized firmware build workflow"`

---

### Task 6: Bridge to OTA publishing/index refresh

**Files:**
- Modify: `.gitea/workflows/firmware-build.yml`
- Modify: `scripts/publish-firmware-release.sh`
- Create: `docs/ci/firmware-release-flow.md`

**Step 1: Add release publish stage**
- Map CI build artifacts into OTA versioned layout.

**Step 2: Regenerate + validate index**
- Run `build-firmware-index.js` + validator in CI.

**Step 3: Add gating**
- Prevent publish on failed builds or invalid index.

**Step 4: End-to-end test**
- Confirm new version appears in `/ota/index.json` and web flasher release dropdown.

**Step 5: Commit**
- `git add` workflow/scripts/docs.
- `git commit -m "feat(ci): publish firmware artifacts to OTA + refresh index"`

---

### Task 7: Operations and maintenance

**Files:**
- Create: `docs/runbooks/gitea-runner-ops.md`

**Step 1: Document common ops commands**
- Restart runner, inspect logs, rotate registration, clear cache.

**Step 2: Document incident playbook**
- Runner offline, artifact upload failure, cache corruption.

**Step 3: Commit**
- `git add docs/runbooks/gitea-runner-ops.md`
- `git commit -m "docs(ci): add gitea runner operations runbook"`
