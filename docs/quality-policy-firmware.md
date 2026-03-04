# Firmware Quality Policy (Phased Rollout)

This document defines the firmware lint/static-analysis rollout for Bramble in low-risk phases.

## Goals

- Improve code hygiene without blocking firmware delivery.
- Keep early checks low-noise and actionable.
- Promote checks from advisory to required only after burn-in.

## Phase map

### Phase 2.1: baseline + local wrappers (advisory)

Added baseline config and local commands:

- `.clang-format` — conservative style baseline
- `.clang-tidy` — low-noise C/C++ checks, no warnings-as-errors
- `.shellcheckrc` — shell lint baseline
- `.markdownlint-cli2.yaml` — docs lint baseline
- `.actionlint.yaml` — workflow lint baseline

Local wrapper scripts:

- `scripts/lint/run-clang-format-check.sh`
- `scripts/lint/run-clang-tidy-advisory.sh`
- `scripts/lint/run-shellcheck.sh`
- `scripts/lint/run-markdownlint.sh`

### Phase 2.2: CI advisory integration

Firmware advisory checks were wired via `.gitea/workflows/firmware-quality.yml`.

Jobs were non-blocking (`continue-on-error: true`) so results stayed visible without merge friction.

### Phase 2.3 (current): selective required gates

Promoted to **required/blocking** in CI:

- `required-clang-format` → `bash scripts/lint/run-clang-format-check.sh --strict`
- `required-shellcheck` → `bash scripts/lint/run-shellcheck.sh --strict`
- `required-actionlint` → `actionlint -color -oneline -config-file .actionlint.yaml .gitea/workflows/firmware-quality.yml`

Held as **advisory/non-blocking** until noise is reduced:

- `advisory-clang-tidy` (heltec-v3 compile DB)
- `advisory-markdownlint`

Rationale: clang-format/shellcheck/actionlint are stable and reproducible locally with low false-positive rate; broad clang-tidy remains noisy and should not block firmware delivery yet.

### Phase 2.4: expanded static analysis

Widen `clang-tidy` coverage and tighten checks incrementally with suppression strategy and ownership.

## Local verification commands

```bash
# Required checks (strict / blocking behavior)
bash scripts/lint/run-clang-format-check.sh --strict
bash scripts/lint/run-shellcheck.sh --strict
actionlint -color -oneline -config-file .actionlint.yaml .gitea/workflows/firmware-quality.yml

# Advisory checks (non-blocking behavior)
bash scripts/lint/run-clang-format-check.sh
bash scripts/lint/run-shellcheck.sh
bash scripts/lint/run-markdownlint.sh
bash scripts/flash.sh local heltec-v3 build
bash scripts/lint/run-clang-tidy-advisory.sh build-heltec-v3
```

## Rollback levers (temporary de-escalation)

If a required check becomes noisy or breaks developer throughput unexpectedly, temporarily de-escalate while remediation happens:

1. In `.gitea/workflows/firmware-quality.yml`, move the check from `required-*` to `advisory-*` naming for clarity.
2. Add `continue-on-error: true` to the affected job.
3. For wrapper-based checks, remove `--strict` so wrappers return advisory success with findings.
4. Document the reason and owner in the PR, plus a date to re-promote.

Rollback is a short-term safety valve, not a permanent bypass.

## Scope and non-goals

- This phase does **not** change firmware runtime behavior.
- This phase does **not** promote broad clang-tidy/cppcheck to required gates yet.
