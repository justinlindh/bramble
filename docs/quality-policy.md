# Quality Gate Policy

This policy defines which CI checks are blocking vs advisory, plus how advisory checks are promoted safely.

## Required checks (blocking on PR)

- Host tests (`bash test/run_all_tests.sh`)
- RPC contract check (`bash scripts/check-rpc-contract.sh`: `api/openapi.yaml` vs the firmware registry in `main/rpc_methods.c`)
- ShellCheck baseline scope (curated low-noise scripts)
- Actionlint for `.github/workflows/quality.yml`
- Ruff baseline profile (`ruff check scripts --select E9,F63,F7,F82`)
- clang-format check (full strict scope)
- cppcheck (`--error-exitcode=2`, blocking)
- Go simulator integration (`gosim-integration`: builds and tests the simulator)

## Advisory checks (non-blocking)

None currently wired into CI. clang-tidy exists only as a local wrapper
(`scripts/lint/run-clang-tidy-advisory.sh`) and is not part of any workflow.

## Non-PR infra-backed required check

- Board build smoke (`bash scripts/flash.sh local heltec-v3 build`) runs on non-PR events (push to `main` and the standard branch prefixes, or manual dispatch) where the `idf-node` runner is available.

## Promotion (ratchet) criteria

Promote advisory checks to required only after all are true:

1. Baseline is triaged and clean for chosen scope.
2. At least 10 consecutive PR runs are stable.
3. Findings are reproducible locally with documented commands.
4. Maintainer sign-off is recorded in PR/issue.
5. Rollback lever is documented in the same promotion PR.

## Rollback levers

When a newly-required check becomes noisy/unreliable, rollback quickly by either:

- restoring `continue-on-error: true`, or
- shrinking scope back to the prior proven baseline.

Any rollback PR must include rationale (tool regression, false positives, infra instability, etc.).

## Change management

- No sudden strictness jumps without written rationale.
- Favor advisory -> stabilize -> enforce progression.
- Required checks should remain deterministic and fast for normal PR iteration.

## Local verification commands (firmware)

```bash
# Required checks (strict / blocking behavior)
bash scripts/lint/run-clang-format-check.sh --strict
bash scripts/lint/run-shellcheck.sh --strict
actionlint -color -oneline -config-file .actionlint.yaml .github/workflows/firmware-quality.yml

# Advisory checks (non-blocking behavior)
bash scripts/lint/run-clang-format-check.sh
bash scripts/lint/run-shellcheck.sh
bash scripts/lint/run-markdownlint.sh
bash scripts/flash.sh local heltec-v3 build
bash scripts/lint/run-clang-tidy-advisory.sh build-heltec-v3
```
