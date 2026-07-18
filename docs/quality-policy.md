# Quality Gate Policy

This policy defines which CI checks are blocking vs advisory, plus how advisory checks are promoted safely.

Since the pipeline optimization, the many tiny static-analysis jobs are bundled
into a single `Static checks` job (context `Static checks`, in
`firmware-quality.yml`). Its steps preserve each former job's exact command, so
the checks below are unchanged; only their packaging changed. See docs/ci.md for
the full job topology and the always-report contract.

## Required checks (blocking on PR)

- `Static checks` (one pod, `firmware-quality.yml`) runs, as named steps:
  - No internal refs (`bash scripts/lint/check-no-internal-refs.sh`)
  - Strict shellcheck (`bash scripts/lint/run-shellcheck.sh --strict`)
  - Ruff baseline profile (`ruff check scripts --select E9,F63,F7,F82`)
  - Strict clang-format, full scope (`bash scripts/lint/run-clang-format-check.sh --strict`)
  - cppcheck (`--error-exitcode=2`, blocking)
  - RPC contract check (`bash scripts/check-rpc-contract.sh`: `api/openapi.yaml` vs the firmware registry in `main/rpc_methods.c`)
  - Actionlint over the four gating workflow files
- `Host tests` (`bash test/run_all_tests.sh`, `quality.yml`)
- `gosim integration` (builds and tests the simulator, `quality.yml`)
- `Webapp checks` (one pod: lint, typecheck, electron typecheck, unit tests, build, e2e smoke, `webapp-quality.yml`)
- `web-flasher tests` (`node --test web-flasher/`, `webapp-quality.yml`)

## Advisory checks (non-blocking)

- `Emulator suite (advisory)` (`quality.yml`): the merged emulator scenario suite
  plus browser E2E. Runs on every PR for signal but is never required, so it does
  not gate merges. Its browser E2E step is additionally `continue-on-error`.

clang-tidy exists only as a local wrapper
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
