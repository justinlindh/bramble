# Quality Gate Policy (Phase 2)

This policy defines which CI checks are blocking vs advisory, plus how advisory checks are promoted safely.

## Required checks (blocking on PR)

- Host tests (`bash test/run_all_tests.sh`)
- ShellCheck baseline scope (curated low-noise scripts)
- Actionlint for `.gitea/workflows/quality.yml`
- Ruff baseline profile (`ruff check scripts --select E9,F63,F7,F82`)
- clang-format check on changed C/C++ files only

## Advisory checks (non-blocking)

- cppcheck (broad static analysis signal)
- clang-tidy sample run against heltec-v3 compile DB (non-PR infra-backed run)

## Non-PR infra-backed required check

- Board build smoke (`bash scripts/flash.sh local heltec-v3 build`) runs on `push main` / manual dispatch where `idf-node` runner is available.

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
