# Quality Gate Policy

Every CI check gates. There is no advisory tier: a check that runs in CI either
blocks merges or it is fixed or removed. A check that cannot be made reliable
does not get demoted to non-blocking signal; it gets redesigned or deleted
(with the redesign requirement written down where it died).

Since the pipeline optimization, the many tiny static-analysis jobs are bundled
into a single `Static checks` job (context `Static checks`, in
`firmware-quality.yml`). Its steps preserve each former job's exact command, so
the checks below are unchanged; only their packaging changed. See docs/ci.md for
the full job topology and the always-report contract.

## Required checks (blocking on PR)

- `Static checks` (one pod, `firmware-quality.yml`) runs, as named steps:
  - No internal refs (`bash scripts/lint/check-no-internal-refs.sh`)
  - No em dash (`bash scripts/lint/check-no-em-dash.sh`): enforces the CLAUDE.md ban on U+2014 across the tracked tree, excluding only the vendored `node_modules`, `managed_components`, and `simulator/engine/cJSON.c`. There are no first-party exemptions, deliberately
  - Board matrix coverage (`bash scripts/lint/check-board-matrix.sh`: the `board-build-smoke` matrix in `quality.yml` must list exactly the boards `scripts/ci-build-firmware.sh` releases)
  - Strict shellcheck (`bash scripts/lint/run-shellcheck.sh --strict`)
  - Ruff baseline profile (`ruff check scripts --select E9,F63,F7,F82`)
  - Strict clang-format, full scope (`bash scripts/lint/run-clang-format-check.sh --strict`), pinned to the version in `.clang-format-version` (currently 14.0.6); the `Static checks` job asserts the runner's `clang-format --version` matches before running the check, and the wrapper script prints a warning if your local version differs, because unpinned clang-format versions disagree on macro and designated-initializer layout and produce unreproducible findings (issue #161)
  - cppcheck (`--error-exitcode=2`, blocking)
  - RPC contract check (`bash scripts/check-rpc-contract.sh`: `api/openapi.yaml` vs the firmware registry in `main/rpc_methods.c`)
  - Actionlint over the four gating workflow files
- `Host tests` (`bash test/run_all_tests.sh`, `quality.yml`)
- `Parser fuzzing` (`bash test/fuzz/run_fuzz.sh`, `quality.yml`): a 30-second-per-target libFuzzer campaign, under ASan and UBSan, over the wire-frame parsers in `components/packet/packet.c` and the fragment reassembler. Both harnesses start from committed seed corpora (`test/fuzz/corpus/`) built from the host suites' own test vectors. Requires `clang` with libFuzzer on the runner image; the job asserts the toolchain is present and fails hard when it is not, because a skip here would be an advisory check in disguise.
- `Release config` (`node scripts/release/semantic-release-squash-expander.test.cjs`, `quality.yml`): the release-rule scope-gating regression, run against the real `.releaserc.<component>.cjs` files, so a config change that would silently stop every component release fails the PR
- `gosim integration` (builds and tests the simulator, `quality.yml`)
- `Board build smoke (heltec-v3)`, `(tdeck-plus)`, `(heltec-v4)`, `(bramble-pager)` (`quality.yml`): one context per board, each running `bash scripts/flash.sh local <board> build`. `fail-fast: false`, so every board reports its own result. The `firmware or workflows` area gate is on the job's steps rather than on the job, because a false job-level `if:` prevents GitHub from expanding the matrix at all and collapses the four contexts into one check run named with the raw literal `Board build smoke (${{ matrix.board }})`. Step-level gating keeps all four contexts reporting on every PR, so they are safe to require
- `Emulator suite` (`quality.yml`): the merged emulator scenario suite plus
  browser E2E. Uses the collect-then-fail pattern (below), so both suites
  always report and both gate.
- `Webapp checks` (one pod: lint, typecheck, electron typecheck, unit tests, build, e2e smoke, `webapp-quality.yml`)
- `web-flasher tests` (`node --test web-flasher/`, `webapp-quality.yml`)

There is no clang-tidy or markdownlint gate. The `run-clang-tidy-advisory.sh`
and `run-markdownlint.sh` wrappers used to sit in `scripts/lint/` unreferenced
by any workflow, Makefile target, or script, and both defaulted to reporting
findings and then exiting 0, which is exactly the advisory tier this policy
forbids. They were deleted rather than promoted: a real clang-tidy gate needs
an xtensa-capable toolchain and a firmware build in the static pod, and a real
markdownlint gate needs a repo-wide markdown formatting sweep first, tracked
as issue #160. The `.clang-tidy` and `.markdownlint-cli2.yaml` dotfiles stay
because editors and language servers read them directly; run `markdownlint-cli2`
or `clang-tidy` by hand if you want the local signal.

## The collect-then-fail pattern (step-level `continue-on-error`)

Job-level or de-facto advisory checks are forbidden, but step-level
`continue-on-error: true` is allowed as an ERROR-COLLECTION mechanism: when a
job runs several independent suites, giving each suite step an `id` and
`continue-on-error: true` lets one run report every suite's result instead of
stopping at the first failure. The job must then end with a terminal step that
fails the job when any collected step's `outcome` is not `success`, e.g.:

```yaml
- name: Fail if any suite failed
  run: |
    failed=0
    [ "${{ steps.scenarios.outcome }}" = "success" ] || failed=1
    [ "${{ steps.e2e.outcome }}" = "success" ] || failed=1
    exit "$failed"
```

`continue-on-error` without such a terminal gate is an advisory check in
disguise and is not allowed.

## Non-PR infra-backed required checks

None. The board build smoke used to be the only one: it ran on non-PR events
only, and it built only `heltec-v3`, so three of the four shipped board targets
were never built by any automatic run and could break with every required
context green. It is now a four-board matrix that gates PRs like every other
required check.

Written rationale for the strictness jump (required by "Change management"
below): the change trades runner time for the largest gating hole in the
pipeline. It is affordable because ESP-IDF ccache landed first, so the builds
are incremental rather than cold, and `max-parallel: 2` caps how much of the
self-hosted pool the matrix can occupy at once. The rollback lever is to
restore the `github.event_name != 'pull_request'` guard on the job, which
returns the four contexts to post-merge validation while keeping all four
boards covered; shrinking the matrix back to `heltec-v3` is the second, larger
step and would need `scripts/lint/check-board-matrix.sh` relaxed with it. Note
that the rollback lever must be applied to the job's steps, not to the job: a
job-level `if:` on a matrix job stops the matrix expanding and strands the four
required contexts.

## Adding or fixing checks

A new or newly fixed check must be required-grade before it lands:

1. Deterministic or event-driven: no fixed wall-clock windows for real-time
   behavior; wait on the observable event up to a generous budget instead.
   No retry loops to absorb known flakiness.
2. Validated where it will run: for anything timing-sensitive, consecutive
   passing runs on the actual CI runner pods (not a fast local box) before it
   gates.
3. Findings are reproducible locally with documented commands.
4. Rollback lever is documented in the PR that adds or promotes the check.

## Rollback levers

When a required check becomes noisy/unreliable, rollback quickly by either:

- shrinking scope back to the prior proven baseline (e.g. removing the
  specific unreliable test while the rest keeps gating), or
- removing the check entirely, with the redesign it needs written down.

Demoting a check to non-blocking is NOT a rollback lever; the advisory tier
does not exist. Any rollback PR must include rationale (tool regression, false
positives, infra instability, etc.).

## Change management

- No sudden strictness jumps without written rationale.
- Required checks should remain deterministic and fast for normal PR iteration.

## Local verification commands (firmware)

```bash
# Required checks (strict / blocking behavior)
bash scripts/lint/run-clang-format-check.sh --strict
bash scripts/lint/run-shellcheck.sh --strict
bash test/fuzz/run_fuzz.sh                    # same 30s/target budget as CI
FUZZ_SECONDS=600 bash test/fuzz/run_fuzz.sh   # longer local campaign
bash test/fuzz/run_fuzz.sh --regen-corpus     # after a wire-format change
actionlint -color -oneline -config-file .actionlint.yaml .github/workflows/firmware-quality.yml

bash scripts/lint/check-no-internal-refs.sh
bash scripts/lint/check-no-em-dash.sh

# Local-only helpers (not wired into CI)
bash scripts/lint/run-clang-format-check.sh
bash scripts/lint/run-shellcheck.sh
bash scripts/flash.sh local heltec-v3 build
```
