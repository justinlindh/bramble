# Firmware Quality Policy (Phased Rollout)

This document defines the firmware lint/static-analysis rollout for Bramble in low-risk phases.

## Goals

- Improve code hygiene without blocking firmware delivery.
- Keep early checks low-noise and actionable.
- Promote checks from advisory to required only after burn-in.

## Phase map

### Phase 2.1 (this step): baseline + local wrappers (advisory)

Added baseline config and local commands:

- `.clang-format` — conservative style baseline
- `.clang-tidy` — low-noise C/C++ checks, no warnings-as-errors
- `.shellcheckrc` — shell lint baseline
- `.markdownlint-cli2.yaml` — docs lint baseline
- `.actionlint.yaml` — workflow lint baseline (already present)

Local wrapper scripts:

- `scripts/lint/run-clang-format-check.sh`
- `scripts/lint/run-clang-tidy-advisory.sh`
- `scripts/lint/run-shellcheck.sh`
- `scripts/lint/run-markdownlint.sh`

**Behavior:** wrappers are advisory-first and deterministic:

- Tool missing => clear `SKIP` message and success exit.
- Findings => bounded output excerpt and success exit.
- Clean run => `PASS` message and success exit.

This allows adoption without blocking firmware builds.

### Phase 2.2 (current): CI advisory integration

Firmware advisory checks are wired via `.gitea/workflows/firmware-quality.yml`.

CI mapping (all advisory / non-blocking):

- `advisory-clang-format` → `bash scripts/lint/run-clang-format-check.sh`
- `advisory-clang-tidy` (heltec-v3 compile DB) →
  - `bash scripts/flash.sh local heltec-v3 build`
  - `bash scripts/lint/run-clang-tidy-advisory.sh build-heltec-v3`
- `advisory-shellcheck` → `bash scripts/lint/run-shellcheck.sh`
- `advisory-markdownlint` → `bash scripts/lint/run-markdownlint.sh`
- `advisory-actionlint` → `actionlint -color -oneline -config-file .actionlint.yaml .gitea/workflows/*.yml`

All jobs in this phase use `continue-on-error: true` so results remain visible without blocking merges.

### Phase 2.3: selective required gates

Promote proven low-noise checks (for example shellcheck, actionlint, formatting on changed files) to blocking status.

### Phase 2.4: expanded static analysis

Widen `clang-tidy` coverage and tighten checks incrementally with suppression strategy and ownership.

## Local verification commands

```bash
bash scripts/lint/run-clang-format-check.sh
bash scripts/flash.sh local heltec-v3 build
bash scripts/lint/run-clang-tidy-advisory.sh build-heltec-v3
bash scripts/lint/run-shellcheck.sh
bash scripts/lint/run-markdownlint.sh
actionlint -color -oneline -config-file .actionlint.yaml .gitea/workflows/*.yml
```

## Scope and non-goals

- This phase does **not** change firmware runtime behavior.
- This phase does **not** enforce new blocking gates yet.
