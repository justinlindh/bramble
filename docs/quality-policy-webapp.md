# Webapp Quality Policy

This document defines the required vs advisory checks for `.gitea/workflows/webapp-quality.yml`, plus local parity commands and temporary rollback levers.

## Scope

The `webapp-quality` workflow runs only when these paths change:

- `webapp/**`
- `.gitea/workflows/webapp-quality.yml`
- `.actionlint.yaml`

## Required vs advisory mapping

### Required (blocking)

These jobs must pass on PRs:

- `required-lint` → `npm run lint --prefix webapp`
- `required-typecheck` → `npm run typecheck --prefix webapp`
- `required-unit-tests` → `npm run test:unit --prefix webapp`
- `required-build` → `npm run build --prefix webapp`

### Advisory (non-blocking)

These jobs report signal but do not block merge:

- `advisory-e2e-smoke` (`continue-on-error: true`) → `npm run test:e2e:smoke --prefix webapp` after starting the local runtime and waiting for health.

## Local parity commands (match CI)

Run from repo root:

```bash
npm ci --prefix webapp
npm run lint --prefix webapp
npm run typecheck --prefix webapp
npm run test:unit --prefix webapp
npm run build --prefix webapp
```

Advisory smoke parity:

```bash
npm ci --prefix webapp
npm run build --prefix webapp
npm run start --prefix webapp
# in a second shell after health is up:
npm run test:e2e:smoke --prefix webapp
```

## Rollback levers (required -> advisory, temporary)

Use rollback only when a required check becomes unstable (tool regression, flaky infra, widespread false positives) and is blocking normal PR flow.

### How to downgrade temporarily

1. Edit `.gitea/workflows/webapp-quality.yml`.
2. For the affected required job, set `continue-on-error: true` and rename the job to indicate advisory status.
3. Keep all unaffected required jobs blocking.
4. Open a rollback PR with the mandatory metadata below.

### Mandatory rollback metadata

Every rollback PR must include:

- **Reason:** concrete failure mode and links to failing runs/issues.
- **Expiry date:** explicit date when required enforcement is restored or revisited.
- **Follow-up task:** tracked issue/PR to fix root cause and re-promote to required.

### Re-enforcement

Before expiry (or sooner when fixed), remove `continue-on-error`, restore required job naming, and link evidence from stable runs.
