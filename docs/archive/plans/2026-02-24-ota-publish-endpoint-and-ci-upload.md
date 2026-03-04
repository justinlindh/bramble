# OTA Publish Endpoint + CI Upload Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Replace fragile host-path OTA publishing with an authenticated OTA publish API so CI can upload firmware artifacts directly and trigger deterministic `index.json` updates.

**Architecture:** Add a small authenticated publish endpoint to the existing bramblemesh web serving stack (same host that serves `/ota`). CI builds firmware, then POSTs artifacts + metadata to this endpoint. The endpoint validates auth + payload, writes release files atomically under `/ota/<channel>/<version>/<board>/`, regenerates `index.json`, and returns a release summary. This removes runner mount coupling and keeps publishing self-contained.

**Tech Stack:** Existing bramble web stack (nginx + lightweight server helper), Gitea Actions workflows, Node/Python for index generation, HMAC/Bearer auth, OTA static directory at `/home/justin/src/dockers/ota`.

## Status (2026-02-24)
- ✅ Plan complete (Tasks 1–6 closed).
- Endpoint is live at `POST /ota/publish` and CI publish path is endpoint-driven.
- Legacy workflow path is disabled.
- Security controls and operations runbook are documented.

### Completion commits (high signal)
- `2f23ccc` / `e9e4cab` / `386f793`: atomic publish + index + hardening iterations
- `7f3dfc2` / `30f6527`: CI publish wiring + verification + legacy disable
- `fc87e38` / `82a002d`: security controls + idempotency race/crash fixes
- `02f04a7`: security doc
- `93f590c`: endpoint verification/rollback runbook

---

### Task 1: Define publish API contract + auth model

**Files:**
- Create: `docs/ota-publish-api.md`
- Modify: `docs/ota-release-schema.md`
- Test: `test/fixtures/ota-publish-request-valid.json`, `test/fixtures/ota-publish-request-invalid.json`

**Step 1: Write failing contract validation test case doc examples**
- Add invalid fixture missing required fields (`version`, `channel`, required files).

**Step 2: Document endpoint contract**
- `POST /ota/publish`
- Auth: `Authorization: Bearer <OTA_PUBLISH_KEY>` (phase 1)
- Request form-data fields:
  - `version` (semver/tag)
  - `channel` (`stable|dev`)
  - `board` (e.g. `heltec-v3`)
  - files: `bootloader.bin`, `partition-table.bin`, `bramble.bin`
  - optional metadata: `commit`, `run_id`, `published_at`

**Step 3: Document response contract**
- Success JSON: `{ ok:true, release:{...}, indexPath:"/ota/index.json" }`
- Error JSON with status codes: 400/401/409/500.

**Step 4: Update schema doc with semver-tagged filename policy**
- Canonical files remain stable names for flasher compatibility.
- Tagged copies allowed/required: `bramble-<version>.bin`, etc.

**Step 5: Commit**
```bash
git add docs/ota-publish-api.md docs/ota-release-schema.md test/fixtures/ota-publish-request-*.json
git commit -m "docs(ota): define authenticated publish API contract"
```

---

### Task 2: Implement publish endpoint service (host-side)

**Files:**
- Create: `/home/justin/src/dockers/ota-publisher/server.js`
- Create: `/home/justin/src/dockers/ota-publisher/package.json`
- Create: `/home/justin/src/dockers/ota-publisher/.env.example`
- Modify: `/home/justin/src/dockers/docker-compose.yml`
- Test: local curl against `http://127.0.0.1:<publisher-port>/healthz`

**Step 1: Add minimal service scaffold**
- Node HTTP server with multipart handling and JSON responses.

**Step 2: Add auth middleware**
- Require `Bearer` key from env (`OTA_PUBLISH_KEY`).
- Reject unauthorized requests with 401.

**Step 3: Add endpoint + health**
- `GET /healthz` returns `{ok:true}`.
- `POST /ota/publish` accepts payload/files.

**Step 4: Add compose service**
- Mount `/home/justin/src/dockers/ota:/ota` rw.
- Expose internally only (e.g. `127.0.0.1:8091`).

**Step 5: Start and verify**
```bash
cd /home/justin/src/dockers
docker compose up -d ota-publisher
curl -s http://127.0.0.1:8091/healthz
```

**Step 6: Commit**
```bash
git add /home/justin/src/dockers/ota-publisher /home/justin/src/dockers/docker-compose.yml
git commit -m "feat(ota): add authenticated ota publish endpoint service"
```

---

### Task 3: Implement atomic file write + index rebuild in publisher

**Files:**
- Modify: `/home/justin/src/dockers/ota-publisher/server.js`
- Reuse: `/home/justin/src/bramble/scripts/build-firmware-index.js` logic (ported or invoked)
- Test: upload request with sample files

**Step 1: Validate required files and metadata**
- Must include the 3 required bins.
- Reject empty/invalid channel/version.

**Step 2: Write release atomically**
- Stage files under temp dir.
- Move to final: `/ota/<channel>/<version>/<board>/...`
- Write both canonical + semver-tagged copies.

**Step 3: Compute checksums/sizes and `.meta.json`**
- For canonical and tagged file copies.

**Step 4: Rebuild index atomically**
- Generate full release list by scanning `/ota`.
- Write `index.json.tmp`, then rename to `index.json`.

**Step 5: Return response with published release details**
- Include artifact file URLs and checksums.

**Step 6: Commit**
```bash
git add /home/justin/src/dockers/ota-publisher/server.js
git commit -m "feat(ota): atomic publish writes and deterministic index rebuild"
```

---

### Task 4: Add CI upload step to call publish endpoint

**Files:**
- Modify: `/home/justin/src/bramble/.gitea/workflows/firmware-build.yml`
- Modify: `/home/justin/src/bramble/.gitea/workflows/firmware-publish-ota.yml` (optional removal/deprecation)
- Create: `/home/justin/src/bramble/scripts/ci-publish-ota.sh`

**Step 1: Add publish helper script**
- Script calls endpoint with curl multipart form-data.
- Reads key from secret/env.
- Fails hard on non-2xx.

**Step 2: Wire workflow step after successful build**
- `if: success()` publish step.
- Post publish, fetch `/ota/index.json` and verify release exists.

**Step 3: Remove/disable fragile legacy publish path**
- Keep one source of truth publish path (endpoint-driven).

**Step 4: Commit**
```bash
git add .gitea/workflows/firmware-build.yml .gitea/workflows/firmware-publish-ota.yml scripts/ci-publish-ota.sh
git commit -m "ci(ota): publish firmware artifacts via authenticated ota endpoint"
```

---

### Task 5: Security hardening for publish endpoint

**Files:**
- Modify: `/home/justin/src/dockers/ota-publisher/server.js`
- Modify: `/home/justin/src/dockers/bramblemesh/nginx.conf` (if reverse proxying endpoint)
- Create: `/home/justin/src/bramble/docs/ota-publish-security.md`

**Step 1: Add request limits**
- Max payload size and timeout.

**Step 2: Add replay/basic abuse guards**
- Optional timestamp window + idempotency key (`version+board+channel`).

**Step 3: Add endpoint exposure policy**
- Keep endpoint unlinked and path-obscured if desired.
- Optionally restrict by source CIDR/IP if CI source is stable.

**Step 4: Add audit logging**
- Log run id/version/channel/board and publish result.

**Step 5: Commit**
```bash
git add /home/justin/src/dockers/ota-publisher/server.js /home/justin/src/dockers/bramblemesh/nginx.conf docs/ota-publish-security.md
git commit -m "sec(ota): harden publish endpoint auth and request controls"
```

---

### Task 6: End-to-end verification and rollback runbook

**Files:**
- Create: `/home/justin/src/bramble/docs/runbooks/ota-publish-endpoint-runbook.md`

**Step 1: Trigger firmware build CI manually**
- Verify build succeeds.
- Verify publish step succeeds.

**Step 2: Verify OTA outputs**
- `/ota/index.json` contains latest release first.
- Canonical + semver-tagged files exist.
- `https://bramblemesh.org/ota/index.json` returns expected release.

**Step 3: Verify web flasher integration**
- Release dropdown shows newest version.
- Board resolves correct canonical file set.

**Step 4: Document rollback**
- Disable publish step in workflow.
- Stop `ota-publisher` service.
- Restore prior `index.json` from backup.

**Step 5: Commit**
```bash
git add docs/runbooks/ota-publish-endpoint-runbook.md
git commit -m "docs(ota): add publish endpoint verification and rollback runbook"
```
