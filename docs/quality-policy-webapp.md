# Webapp Quality Policy

This document defines the required vs advisory checks for `.github/workflows/webapp-quality.yml`, plus local parity commands and temporary rollback levers.

## Scope

The `webapp-quality` workflow runs on every push to a tracked branch (no
workflow-level path filter, so every job always reports a context; see
[ci.md](ci.md) for why). Each job still skips its heavy work unless the diff
touches its area:

- `webapp/**` changes: all jobs except `web-flasher tests` run.
- `web-flasher/**` changes: `web-flasher tests` runs.
- Any `.gitea/workflows/**` or `.actionlint.yaml` change: every job runs,
  regardless of area, so a changed workflow definition always gets
  exercised.

## Required vs advisory mapping

### Required (blocking)

These jobs must pass on PRs:

- `required-lint` → `npm run lint --prefix webapp`
- `required-typecheck` → `npm run typecheck --prefix webapp`
- `typecheck-electron` → `npm run typecheck:electron --prefix webapp`
- `required-unit-tests` → `npm run test:unit --prefix webapp`
- `required-build` → `npm run build --prefix webapp`
- `required-e2e-smoke` → `npm run test:e2e:smoke --prefix webapp` (script handles startup, bounded health wait, endpoint assertions, and fail-fast diagnostics; CI sets `SMOKE_MAX_WAIT_SECONDS=90` for deterministic startup budget)

### Advisory (non-blocking)

No advisory lanes are currently configured in this workflow.

## Local parity commands (match CI)

Run from repo root:

```bash
npm ci --prefix webapp
npm run lint --prefix webapp
npm run typecheck --prefix webapp
npm run typecheck:electron --prefix webapp
npm run test:unit --prefix webapp
npm run build --prefix webapp
```

Required e2e smoke parity:

```bash
npm ci --prefix webapp
npm run build --prefix webapp
npm run test:e2e:smoke --prefix webapp
```

Notes:
- `test:e2e:smoke` starts/stops the unified runtime by default.
- Managed start mode fails fast if the target port is already occupied (prevents false-positive passes against an unrelated runtime).
- Set `SMOKE_START_RUNTIME=0` only when intentionally testing an already-running runtime.

## Rollback levers

There is no advisory tier: a required check that becomes unstable (tool
regression, flaky infra, widespread false positives) is FIXED or REMOVED,
never demoted to non-blocking. `continue-on-error` without a terminal gate is
forbidden (see docs/quality-policy.md's collect-then-fail pattern).

### How to roll back

1. Prefer shrinking scope to the prior proven baseline (e.g. remove the
   specific unreliable test while the rest keeps gating).
2. If the whole check is unsalvageable in place, delete it from the workflow
   with the redesign it needs written down where it died.

### Mandatory rollback metadata

Every rollback PR must include:

- **Reason:** concrete failure mode and links to failing runs/issues.
- **Follow-up task:** tracked issue/PR to fix the root cause and restore the
  check.

### Restoration

A restored check returns as a required check from its first run, with
evidence from stable runs linked in the restoring PR.