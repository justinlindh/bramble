# OTA Hosting + Versioned Web Flasher Releases Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Host firmware artifacts on the GPU box behind an unlinked URL and update the existing `web-flasher` to fetch and present versioned firmware releases (newest first) automatically from Gitea-backed build outputs.

**Architecture:** Use a static firmware index (`index.json`) + versioned artifact directories served from the existing web server stack under a dark, unlinked OTA path (e.g. `/ota/`). CI/build automation writes versioned artifacts and updates the index. Existing `web-flasher` reads that index and drives board-specific flashing from selected release assets.

**Tech Stack:** Existing Bramble web flasher (`web-flasher`), Docker Compose on GPU box, static HTTP hosting (nginx), Gitea CI/artifact publishing, JSON release index.

## Status (2026-02-24)
- ✅ Completed through **Task 6**.
- Task 5/6 were finalized using the focused implementation plan:
  - `docs/plans/2026-02-24-ota-publish-endpoint-and-ci-upload.md`
- Effective outcomes now in place:
  - Endpoint-driven CI publish (`POST /ota/publish`) is the source of truth.
  - Deterministic OTA index updates and release verification in workflow.
  - Legacy fragile publish workflow disabled.
  - Security controls + publish/rollback runbook documented.
- Next work should proceed from follow-on OTA/web flasher priorities, not this plan.

---

### Task 1: Define firmware artifact schema + release index contract

**Files:**
- Create: `docs/ota-release-schema.md`
- Modify: `web-flasher/README.md`
- Test: `scripts/validate-firmware-index.js`

**Step 1: Write failing schema validation test**
- Create a Node script that validates a sample `index.json` and exits non-zero if required fields are missing.

**Step 2: Run test to verify it fails with empty/malformed sample**
- Run: `node scripts/validate-firmware-index.js test/fixtures/firmware-index-invalid.json`
- Expected: non-zero + missing fields report.

**Step 3: Define minimal schema in docs**
- Include required fields per release entry:
  - `version`
  - `published_at`
  - `channel` (stable/dev)
  - `artifacts[]` with `board`, `file`, `sha256`, `size`, optional `notes`
- Define sort behavior: latest first by `published_at` then semantic version.

**Step 4: Add valid fixture + pass validator**
- Add `test/fixtures/firmware-index-valid.json` and confirm validator passes.

**Step 5: Commit**
- `git add docs/ota-release-schema.md scripts/validate-firmware-index.js web-flasher/README.md test/fixtures`
- `git commit -m "docs(ota): define firmware release index contract + validator"`

---

### Task 2: Serve dark OTA path from existing web server stack (unlinked URL)

**Files:**
- Modify: existing nginx/site config in `/home/justin/src/dockers` (current serving stack)
- Create: OTA artifact root directory under existing served content (e.g. `.../ota/`)
- Create: `docs/ota-dark-url.md`
- Test: GPU-box `curl` checks for `/ota/index.json` and sample bins

**Step 1: Add `/ota/` route in existing server config**
- Serve OTA artifact root read-only from current web server.
- Keep URL unlinked from main/public UI.

**Step 2: Add cache/content-type behavior**
- Ensure JSON + bin content-types are correct.
- Add long cache headers for binaries, shorter/no-cache for `index.json`.

**Step 3: Reload existing stack and validate locally**
- Reload/restart existing web server service only.
- `curl http://127.0.0.1:<existing-web-port>/ota/index.json`

**Step 4: Document dark URL policy**
- Unlinked URL only, no navigation links from public site.

**Step 5: Commit**
- `git add` modified server config + docs.
- `git commit -m "feat(ota): serve dark /ota path from existing web stack"`

---

### Task 3: Publish versioned artifacts + index update script

**Files:**
- Create: `scripts/publish-firmware-release.sh`
- Create: `scripts/build-firmware-index.js`
- Create: `scripts/sha256-artifacts.sh`
- Test: local dry-run against sample artifact directory

**Step 1: Write failing dry-run test**
- Run publish script against missing input and assert meaningful error.

**Step 2: Implement publish script**
- Inputs: version, channel, source artifact paths.
- Output layout:
  - `/ota/<channel>/<version>/<board>/<files>`
  - updated `/ota/index.json`

**Step 3: Implement checksum + size collection**
- Generate `sha256`, `size`, and file names per artifact.

**Step 4: Build index generation script**
- Rebuild `index.json` from directory contents deterministically.
- Sort latest first.

**Step 5: Verify with validator**
- `node scripts/validate-firmware-index.js /path/to/ota/index.json`

**Step 6: Commit**
- `git add scripts/publish-firmware-release.sh scripts/build-firmware-index.js scripts/sha256-artifacts.sh`
- `git commit -m "feat(ota): publish versioned artifacts and generate firmware index"`

---

### Task 4: Integrate existing `web-flasher` with release index

**Files:**
- Modify: `web-flasher/flasher.js`
- Modify: `web-flasher/index.html`
- Modify: `web-flasher/styles.css` (if present)
- Test: browser/manual + mocked index fetch

**Step 1: Write failing JS test/smoke script for parser**
- Add a small script validating parse + sort of release entries.

**Step 2: Add release index fetch**
- Configurable base URL for OTA endpoint.
- Fetch `/index.json`, parse, validate minimally.

**Step 3: Add release selector UI**
- Version dropdown (latest first), channel filter optional.
- Board-specific artifact auto-selection (existing board selection reused).

**Step 4: Add artifact resolution logic**
- Given board + version, resolve bin set required for flash.
- Show checksum/metadata in UI before flash.

**Step 5: Add robust error handling**
- Missing version, missing board artifacts, network failures.

**Step 6: Commit**
- `git add web-flasher/flasher.js web-flasher/index.html`
- `git commit -m "feat(web-flasher): load versioned firmware list from OTA index"`

---

### Task 5: CI/Gitea automation hook for release publishing

**Files:**
- Modify: existing Bramble CI workflow in repo (Gitea Actions equivalent)
- Create: `docs/ota-publish-pipeline.md`
- Test: dry-run pipeline invocation

**Step 1: Add pipeline stage after successful firmware build**
- Collect board bins from build outputs.
- Derive version/tag metadata.

**Step 2: Call publish script**
- Publish to OTA host storage path (via mounted volume/SSH/rsync as appropriate).

**Step 3: Rebuild index and validate in pipeline**
- Fail pipeline if index invalid.

**Step 4: Add deployment notes**
- Secrets/env needed for publishing.
- Rollback procedure.

**Step 5: Commit**
- `git add` pipeline + docs.
- `git commit -m "ci(ota): auto-publish firmware releases and refresh index"`

---

### Task 6: Verification and rollout

**Files:**
- Create: `docs/runbooks/ota-rollout-checklist.md`
- Test: end-to-end from CI artifact -> index -> web flasher selection

**Step 1: Run end-to-end verification**
- Build test release.
- Confirm `index.json` includes release.
- Confirm web flasher shows version at top.

**Step 2: Confirm board mapping accuracy**
- Validate each supported board resolves correct binaries.

**Step 3: Confirm dark URL posture**
- Unlinked from main site.
- Reachable only by direct URL.

**Step 4: Document operational commands**
- Publish manual release.
- Rebuild index.
- Rollback index/artifacts.

**Step 5: Commit**
- `git add docs/runbooks/ota-rollout-checklist.md`
- `git commit -m "docs(ota): add rollout + operations checklist"`

---

## Notes specific to your current constraint
- Reuse existing `web-flasher` in `web-flasher/` (no replacement app).
- OTA host remains “dark” by policy (unlinked URL), not necessarily access-restricted for now.
- Prioritize deterministic `index.json` generation so flasher behavior is stable and debuggable.
