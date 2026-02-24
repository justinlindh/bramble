# Web Flasher Consumer Redesign — Execution Plan

## Goal
Ship a consumer-first web flasher UI with hidden advanced controls, strict all-or-nothing release visibility, and CI verification aligned to the same release-completeness contract.

## Status (2026-02-24)
- ✅ Plan complete (Task A + Task B delivered and live).
- Web flasher is now simple-first with Advanced collapsed by default.
- CI now enforces complete 3-board publish contract and verifies canonical artifacts.
- OTA version source now maps to firmware semver constant (`BRAMBLE_VERSION_STR`) for accurate flasher versions.

### Completion commits (high signal)
- `3ba82cd`: simple-first web flasher redesign + advanced panel + completeness filtering
- `89ba7d2`: CI completeness verification hardening
- `95b885b`: stable→dev fallback when no complete stable releases
- `706892e`: OTA publish version sourced from firmware semver constant
- `a64f202`: CI index verifier fix for `.file` canonical artifact checks

## Product constraints (locked)
- Simple mode first for normal consumers.
- Advanced panel is optional/collapsed.
- No server-side paths in user-visible logs.
- Release is valid only if it includes all supported boards.
- Supported boards: `heltec-v3`, `tdeck-plus`, `heltec-v4`.
- CI must fail hard on partial publish.

## Task breakdown

### Task A — Web flasher UX + completeness filtering
**Ownership:** `web-flasher/*` + `web-flasher/README.md`

1. Remove refresh button and simplify default view.
2. Replace verbose log area with compact status messaging.
3. Add collapsed Advanced panel with:
   - channel selector (`stable|dev`)
   - release selector
4. Persist last-selected board in localStorage (default simple mode behavior).
5. Implement release completeness filter:
   - show/select only releases with required canonical artifacts for all supported boards.
6. Ensure no path leakage in UI text/log output.
7. Keep flashing flow functional for selected board/release.

**Deliverable:** Commit with web-flasher UI + logic updates.

---

### Task B — CI hardening to enforce completeness contract
**Ownership:** `.gitea/workflows/firmware-build.yml` + `scripts/*` tests if needed

1. Keep board build/publish matrix for `heltec-v3`, `tdeck-plus`, `heltec-v4`.
2. Strengthen post-publish verification to assert for each board in release:
   - canonical `bootloader.bin`
   - canonical `partition-table.bin`
   - canonical `bramble.bin`
3. Fail workflow if any required board/file missing.
4. Keep normalized version handling consistent with publish script.

**Deliverable:** Commit with CI verification hardening.

## Verification checklist
- `https://bramblemesh.org/web-flasher/` shows simple-first UX.
- Advanced controls are hidden/collapsed by default.
- No server-path strings shown in UI text.
- Release list excludes partial/incomplete releases.
- CI run for latest commit publishes all 3 boards and passes verification.
- `https://bramblemesh.org/ota/index.json` latest release contains all three boards and required canonical artifacts.

## Execution strategy
- Dispatch two codex subagents with exclusive file ownership.
- Integrate Task A and Task B commits.
- Run end-to-end verification on live endpoint.
- Report only when everything is complete.