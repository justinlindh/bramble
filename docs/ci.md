# CI: always-run workflows, per-job skips, change-based gating

This page explains how the PR-gating GitHub Actions workflows are structured so
that every required context always reports a status, while only the jobs
relevant to a change actually execute.

## The workflows

Three workflows gate pull requests, plus one reusable helper they all call:

| Workflow | Role |
| --- | --- |
| `_detect-changes.yml` | Reusable (`workflow_call`) change-detection. Emits boolean area outputs. Never gates anything; called once per gating workflow. |
| `firmware-quality.yml` | The single-pod `Static checks` bundle (every cheap lint/static check) plus the change detector. |
| `quality.yml` | The heavy firmware/emulator compute jobs: host tests, gosim, the board build smoke, and the advisory emulator suite. |
| `webapp-quality.yml` | The consolidated webapp job (one `npm ci`, all webapp checks) plus the web-flasher tests. |

Publish-oriented workflows (`firmware-build.yml`, `firmware-publish-ota.yml`,
`ci-smoke-artifacts.yml`, `release-components.yml`, `webapp-build-publish.yml`,
`desktop-build.yml`, `claude.yml`) are out of scope here; they run on
`workflow_dispatch`, tags, or their own events and do not gate normal PRs.

## Trigger rules: the PR is the single gate

All three gating workflows use the same `on:` block:

```yaml
on:
  workflow_dispatch:
  pull_request:
  push:
    branches:
      - main
```

`pull_request` fires once per PR (and on every update to the PR head). `push`
fires only on `main`, i.e. post-merge, which is where the post-merge-only jobs
(the board build smoke) validate. There is no per-branch `push` trigger, so a
PR gets exactly one wave of checks instead of two overlapping waves (a
`pull_request` wave and a `push`-to-`fix/**` wave) that previously each ran to
completion in separate concurrency groups and doubled the queue. `webapp-quality.yml`
gained the `pull_request` trigger it previously lacked, so it now gates PRs the
same way as the other two.

Each workflow sets `concurrency` keyed on the ref with
`cancel-in-progress: ${{ github.event_name == 'pull_request' }}`, so a new push
to a PR cancels the superseded PR run but never cancels a `main` run.

## The always-report contract (do not break this)

Branch protection's required-status-checks feature blocks a merge until a named
context reports success. It cannot tell "this check has not run yet" from "this
check will never run"; both look identical and both block the PR forever.

The rule that keeps this safe:

> Workflows always TRIGGER. Heavy jobs SKIP via job-level `if:` conditions on
> the detector's outputs. A skipped required job still produces a passing
> context, so every required check always reports.

Concretely:

1. No gating workflow has a workflow-level `paths:` filter. Every matching
   `pull_request` / `push`-to-`main` event triggers the whole workflow. A
   `paths:` filter would strand a required context forever the moment a diff
   misses its paths, so it is banned on anything required.
2. A cheap `detect` job (the reusable `_detect-changes.yml`) runs first and
   emits area booleans.
3. Every real job declares `needs: detect` and an `if:` on the relevant
   `needs.detect.outputs.*` area(s). When the condition is false, Actions still
   creates the job and its check run and skips straight to a `skipped`
   conclusion. Job-level `if:` never changes the context string, so the
   required context still reports.

The single exception is the `Static checks` bundle, which has no `if:` at all
and therefore runs on every trigger (see below).

## The reusable detector

`_detect-changes.yml` is a `workflow_call` reusable workflow. Each gating
workflow calls it once as its `detect` job:

```yaml
jobs:
  detect:
    name: Detect changed areas
    uses: ./.github/workflows/_detect-changes.yml
```

This replaces the three near-identical copies of the detection script that used
to live inline in each workflow with one definition. It still runs once per
caller (three detector pods per PR wave, one per workflow), but the diff-to-area
mapping now lives in exactly one place.

Its `Compute changed areas` step resolves a git diff range in this order:

1. `pull_request`: fetch `origin/<base_ref>` and diff `origin/<base_ref>...HEAD`.
2. `push` with a non-zero `before` SHA: diff `<before>..HEAD`.
3. Anything else (new-branch push with an all-zero `before`, `workflow_dispatch`,
   or an unresolved base): fetch `origin/main`, take the merge base with `HEAD`,
   diff `<merge-base>...HEAD`.

If none resolve, it does not fail; it logs a warning and marks every area
changed. When in doubt, run the real checks rather than silently skip them.

### Areas and their patterns

| Output | Marks changed when the diff touches |
| --- | --- |
| `firmware` | `main/`, `components/`, `test/`, `api/`, `scripts/`, `CMakeLists.txt`, `sdkconfig.*`, `partitions*.csv`, `.clang-format`, `.clang-tidy`, `.shellcheckrc`, `.markdownlint-cli2.yaml` |
| `simulator` | `simulator/` |
| `emulator` | `emulator/` |
| `webapp` | `webapp/` |
| `web_flasher` | `web-flasher/` |
| `workflows` | any file under `.github/workflows/`, or `.actionlint.yaml` |
| `docs` | `docs/`, `README.md` |

`firmware` is deliberately a superset of every firmware-area consumer's real
inputs (host tests, board build, ruff, cppcheck, rpc-contract, and the strict
clang-format/shellcheck/markdownlint wrappers). `simulator` is a separate output
because the strict clang-format scope scans simulator C sources too, so a
simulator-only change must still run clang-format (inside the bundle) and gosim.

### The safety rule: workflow edits force everything

`workflows` is OR'd into every job's `if:`. So editing any file under
`.github/workflows/` (or `.actionlint.yaml`) runs every job in all three
workflows. Editing a job's steps is itself a change that must be exercised; if a
workflow edit only ran jobs whose code area also happened to change, a broken
job definition could land unverified. This over-triggers on rare
workflow-edit PRs on purpose, in exchange for a rule that is impossible to
under-match.

### The area superset rule

Every job's area set must be a superset of what its commands actually read. When
a script widens its scan scope, widen the matching area regex or job condition
with it, or PRs touching only the new scope will falsely skip the check. Audit
by reading the scripts, not the job names: `run-clang-format-check.sh --strict`
scans `main/ components/ test/ simulator/`, `run-shellcheck.sh --strict` scans
`scripts/lint/*.sh`, cppcheck scans `main components`, the host test suite builds
`test/` against `components/` and `main/`, and the board build additionally reads
root `CMakeLists.txt`, `sdkconfig.defaults*`, and `partitions*.csv`.

## Job topology

### `firmware-quality.yml`

| Job (context name) | Runs when | Required? |
| --- | --- | --- |
| `Detect changed areas` (via reusable `detect`) | always | no |
| `Static checks` | always (no `if:`) | yes |

`Static checks` is one pod, one checkout, and a sequence of named steps that each
preserve a former standalone job's exact command: no-internal-refs, strict
shellcheck, ruff baseline, strict clang-format, cppcheck, the rpc-contract check,
and actionlint over the four gating workflow files. Each check is its own step so
a failure attributes to the exact tool in the UI. It has no `if:` because it is
THE universal gate: it must run (and report) on every PR, including a docs-only
PR where it is the only job that executes. The individual checks are cheap and
pass trivially when their scope did not change.

### `quality.yml`

| Job (context name) | Runs when | Required? |
| --- | --- | --- |
| `Detect changed areas` (via reusable `detect`) | always | no |
| `Host tests` | `firmware` or `workflows` | yes |
| `gosim integration` | `firmware`, `simulator`, or `workflows` | yes |
| `Board build smoke (heltec-v3)` | not a `pull_request` event, and (`firmware` or `workflows`) | post-merge required (see quality-policy.md) |
| `Emulator suite (advisory)` | `firmware`, `simulator`, `emulator`, or `workflows` | no (advisory) |

`Emulator suite (advisory)` merges the former `emulator-scenarios` and
`emulator-e2e` jobs into one job that builds the linux firmware node, gosim, and
the UI once, then runs the headless scenario suite followed by the browser E2E.
It now runs on PRs too (it used to be gated off `pull_request`), giving signal
on every change while staying non-required so it never blocks a merge. Its two
scenarios run in parallel inside `emulator/ci/run_scenarios.sh` (each gosim
process keys its socket path and node-state dir to its own PID, so there is no
port or state collision) with trimmed retry budgets. The browser E2E step is
`continue-on-error` while it bakes, so an E2E failure does not fail even the
advisory job.

`Board build smoke` keeps its `github.event_name != 'pull_request'` guard: the
ESP-IDF board build is heavy and validates post-merge on pushes to `main`. It
therefore cannot be a PR-required check; it is the one non-PR required check.

### `webapp-quality.yml`

| Job (context name) | Runs when | Required? |
| --- | --- | --- |
| `Detect changed areas` (via reusable `detect`) | always | no |
| `Webapp checks` | `webapp` or `workflows` | yes |
| `web-flasher tests` | `web_flasher` or `workflows` | yes |

`Webapp checks` is one job with a single `npm ci`, then lint, typecheck, electron
typecheck, unit tests, build, and the e2e smoke run as sequential steps. The smoke
run reuses the build produced earlier in the same job, so there is no second
`npm ci` and no second build. `web-flasher tests` stays separate because it runs
`node --test web-flasher/` with no webapp install.

## What a docs-only PR looks like now

A PR that touches only `docs/**` (and nothing under `main/`, `components/`,
`test/`, `api/`, `scripts/`, `simulator/`, `emulator/`, `webapp/`,
`web-flasher/`, or `.github/workflows/`) still triggers all three workflows. The
three `detect` jobs run (fast: a diff, no build), the `Static checks` bundle runs
(one pod), and every other job reports `skipped`. Every context branch protection
could require gets a report either way, so the PR is never stuck waiting on a
check that was never going to run.

## Adding a new job

1. Add `needs: detect`.
2. Add an `if:` referencing the relevant `needs.detect.outputs.*` area(s), OR'd
   with `workflows` at minimum. (The `Static checks` bundle is the sole
   deliberate exception: no `if:`, so it always runs and reports.)
3. If the job needs an area the detector does not yet compute, add a new output
   and pattern to `_detect-changes.yml` and document it in the table above.
4. Do not add a workflow-level `paths:` filter. It reintroduces the exact
   deadlock this structure removes.

## Dual-tree arrangement: .github is authoritative, .gitea is a frozen mirror

`.github/workflows/` is the source of truth for CI and runs on the self-hosted
ARC scale set (runner label from the `RUNNER_LABEL` repo variable).
`.gitea/workflows/` is a frozen, unmodified mirror-side copy and is not touched
by workflow edits. The publish-oriented workflows still carry Gitea API coupling
and are gated to `workflow_dispatch` until a Phase 2 pass rewrites them for
GitHub natively; each carries a `PHASE-2 PORT PENDING` header.
