# CI: always-run workflows, per-job skips

This page explains how the three PR-gating Gitea Actions workflows
(`quality.yml`, `firmware-quality.yml`, `webapp-quality.yml`) are structured
so that every job they define always reports a status context, and why that
structure exists.

## The problem this solves

Branch protection's "required status checks" feature blocks a merge until a
named context reports `success` (or, depending on configuration, `skipped`).
It cannot distinguish "this check has not run yet" from "this check will
never run": both look identical from the server's point of view, and both
permanently block the PR.

Before this change, `quality.yml`, `firmware-quality.yml`, and
`webapp-quality.yml` each had a workflow-level `paths:` filter. When a diff
did not touch any listed path, Gitea Actions never triggered the workflow at
all, so none of its jobs ever created a check run. A PR that only touched,
say, `hardware/` or `CLAUDE.md` would never get a `Host tests` or `Required
lint` context, and if those contexts were marked required, the PR could
never merge. That deadlock is what motivated this restructure; it is also
why these three workflows previously could not be used as the basis for
required checks at all.

## The fix

1. Remove the workflow-level `paths:` filter from all three workflows. Every
   push/pull_request event that matches the `on:` branch patterns now
   triggers the workflow, full stop. The event set (`workflow_dispatch`,
   `pull_request`, `push` with its branch list) is otherwise unchanged.
2. Add a cheap first job, `changes` (job name `Detect changed areas`), to
   each workflow. It checks out full history (`fetch-depth: 0`), computes a
   diff range against the right base for the triggering event, and emits a
   handful of boolean outputs describing which areas of the repo the diff
   touches.
3. Every other job in the workflow gets `needs: changes` plus an `if:`
   condition on the relevant output(s). When the condition is false, Gitea
   Actions still creates the job and its check run, it just skips straight
   to a `skipped` conclusion without running any steps. The job's identity
   (and therefore its context string) is unaffected by `if:` at the job
   level; only workflow-level filters or event non-matches can prevent a job
   from being created at all, so job-level `if:` is the only mechanism
   compatible with required checks.

This mirrors a pattern already in use before this change: `quality.yml`'s
`board-build-smoke` and `emulator-*` jobs have long used
`if: github.event_name != 'pull_request'` to skip the two self-hosted
`idf-node` jobs on PR events while still reporting a `skipped` context. This
restructure generalizes that same mechanism (job-level `if:`, not
workflow-level `paths:`) to every job, and combines it with diff-derived
area booleans instead of only the event type.

## The context naming contract

Every job keeps its exact pre-existing `name:`. That name is the check-run
context string branch protection matches against. Renaming a job breaks any
required-check rule that names it, so job names are treated as a stable
public contract from this point forward. The `changes` job itself
(`Detect changed areas`) is new in all three workflows and is not intended
to be a required context; it exists purely to feed the other jobs.

## How the diff range is resolved

The `changes` job's `Compute changed areas` step resolves a git diff range
in this order:

1. `pull_request` events: fetch `origin/<base_ref>` and diff
   `origin/<base_ref>...HEAD`.
2. `push` events with a non-zero `before` SHA (the normal case: a fast
   forward from the branch's previous tip): diff `<before>..HEAD`.
3. Anything else, including a `push` whose `before` SHA is all zeros (a
   brand new branch's first push) and `workflow_dispatch`: fetch
   `origin/main`, take the merge base with `HEAD`, and diff
   `<merge-base>...HEAD`.

If none of these can be resolved (for example `origin/main` cannot be
fetched), the job does not fail; it logs a warning and treats every area as
changed, which is the safe default: when in doubt, run the real checks
instead of silently skipping them.

## Skip conditions per job

### `quality.yml`

Areas: `firmware` (`main/`, `components/`, `test/`, `api/`, `scripts/`,
`CMakeLists.txt`, `sdkconfig.*`, `partitions*.csv`, `.clang-format`,
`.shellcheckrc`), `simulator` (`simulator/`), `emulator` (`emulator/`),
`workflows` (any file under `.gitea/workflows/`, plus `.actionlint.yaml`),
`docs` (`docs/`, `README.md`, computed but not currently consumed by any job
in this file).

| Job | Runs when |
| --- | --- |
| `Host tests` | `firmware` or `workflows` |
| `ShellCheck` | `firmware` or `workflows` |
| `RPC contract (spec vs firmware registry)` | `firmware` or `workflows` |
| `Actionlint` | `workflows` |
| `Ruff` | `firmware` or `workflows` |
| `Clang-format` | `firmware`, `simulator`, or `workflows` (the strict scope scans `simulator/` C sources too) |
| `cppcheck` | `firmware` or `workflows` |
| `Board build smoke (heltec-v3)` | not a `pull_request` event, and (`firmware` or `workflows`) |
| `gosim integration` | `firmware`, `simulator`, or `workflows` |
| `Emulator scenario suite` | not a `pull_request` event, and (`firmware`, `simulator`, `emulator`, or `workflows`) |
| `Emulator browser E2E (non-required)` | same as above (also keeps `continue-on-error: true`) |

### `firmware-quality.yml`

Areas: `firmware` (`main/`, `components/`, `test/`, `simulator/`,
`scripts/lint/`, top-level `scripts/*.sh`, `.clang-format`, `.clang-tidy`,
`.shellcheckrc`, `.markdownlint-cli2.yaml`; `test/` and `simulator/` are
included because the strict clang-format scope scans their C sources),
`workflows` (any file under `.gitea/workflows/`, plus `.actionlint.yaml`),
`docs` (computed, not consumed here).

| Job | Runs when |
| --- | --- |
| `Required clang-format` | `firmware` or `workflows` |
| `Required shellcheck` | `firmware` or `workflows` |
| `Required actionlint` | `workflows` |

### `webapp-quality.yml`

Areas: `webapp` (`webapp/`), `web_flasher` (`web-flasher/`), `workflows`
(any file under `.gitea/workflows/`, plus `.actionlint.yaml`).

| Job | Runs when |
| --- | --- |
| `Required lint` | `webapp` or `workflows` |
| `Required typecheck` | `webapp` or `workflows` |
| `Required electron typecheck` | `webapp` or `workflows` |
| `web-flasher tests` | `web_flasher` or `workflows` |
| `Required unit tests` | `webapp` or `workflows` |
| `Required webapp build` | `webapp` or `workflows` |
| `Required e2e smoke` | `webapp` or `workflows` |

Note that `webapp-quality.yml` has no `pull_request:` trigger; it relies on
`push:` events against `fix/**`, `feat/**`, `feature/**`, `chore/**`,
`ci/**`, and `main`, which fire on every push to a PR's source branch. That
event set is unchanged by this restructure.

### Why `workflows` always forces every job

All three workflows use the same broad `workflows` pattern: any file under
`.gitea/workflows/` (or `.actionlint.yaml`) marks the area changed, and the
area is OR'd into every job's condition. So editing any workflow file runs
every job in all three workflows. The point is that editing a job's steps
is itself a change that needs to be exercised; if editing
`webapp-quality.yml` only ran jobs whose code area also happened to change
in the same diff, a broken job definition could land unverified. The broad
pattern deliberately over-triggers (editing `firmware-build.yml` also runs
the webapp jobs); that costs some runner time on rare workflow-edit PRs and
in exchange keeps the rule simple and impossible to under-match.

### The area superset rule

Every job's area set must be a superset of what its commands actually read.
When a script widens its scan scope (for example the strict clang-format
wrapper picking up a new directory), the corresponding area regex or job
condition must widen with it, or PRs touching only the new scope will
falsely skip the check. When auditing, read the scripts, not the job names:
`run-clang-format-check.sh --strict` scans `main/ components/ test/
simulator/`, `run-shellcheck.sh --strict` scans `scripts/lint/*.sh`,
cppcheck scans `main components`, the host test suite builds `test/` against
`components/` and `main/` sources, and the board build additionally reads
root `CMakeLists.txt`, `sdkconfig.defaults*`, and `partitions*.csv`.

### Known gap: idf-node jobs never run on pull_request

The three `idf-node` jobs (`Board build smoke (heltec-v3)`,
`Emulator scenario suite`, `Emulator browser E2E (non-required)`) keep their
`github.event_name != 'pull_request'` condition, so they never run on
pull_request events, even for edits to their own job definitions; they
validate post-merge on pushes to `main` (and on direct branch pushes). This
is pre-existing and deliberate: the self-hosted `idf-node` runner is a heavy
single host that PR volume would saturate.

## What a docs-only PR looks like now

A PR that touches only `docs/**` (and nothing under `main/`, `components/`,
`test/`, `api/`, `scripts/`, `simulator/`, `emulator/`, `webapp/`,
`web-flasher/`, or `.gitea/workflows/`) still triggers all three workflows.
The `changes` job runs (fast: no build, just a diff), and every downstream
job reports `skipped`. Every context branch protection could require from
these three workflows gets a report either way, so the PR is never stuck
waiting on a check that was never going to run.

## Adding a new job

When adding a job to one of these three workflows:

1. Add `needs: changes` (and keep any other existing `needs:`).
2. Add an `if:` referencing the relevant `needs.changes.outputs.*` area(s),
   OR'd with `workflows` at minimum.
3. If the job needs an area this page's `changes` job does not yet compute,
   add a new boolean output and pattern to that job's `Compute changed
   areas` step, and document it in the table above.
4. Do not add a new workflow-level `paths:` filter. It reintroduces the
   exact deadlock this restructure removes.

Server-side enforcement went live on 2026-07-16: branch protection requires the 23 push-event contexts listed above (every job plus each workflow's change-detection job, excluding the explicitly non-required emulator browser E2E). This paragraph doubles as the enforcement probe: it merged through a docs-only PR whose heavy jobs all reported skipped.

## Dual-tree arrangement: .github is authoritative, .gitea is a frozen mirror

As of the GitHub-primary migration, `.github/workflows/` is the source of truth for CI and runs on a self-hosted GitHub Actions runner (labels `self-hosted, linux, bramble-host` for general jobs, `self-hosted, idf-node` for ESP-IDF jobs); `.gitea/workflows/` is kept as a frozen, unmodified mirror-side copy and is not touched by future workflow edits. The three quality workflows and `desktop-build.yml` ported to `.github/workflows/` unchanged in behavior beyond the runner label mapping and updating the `changes` job's own-path pattern from `.gitea/workflows/` to `.github/workflows/`. The publish-oriented workflows (`release-components.yml`, `firmware-publish-ota.yml`, `ci-smoke-artifacts.yml`, `webapp-build-publish.yml`, and the publish half of `firmware-build.yml`) still carry Gitea API coupling (`GITEA_TOKEN`-style secrets, `api/v1` dispatch calls, Gitea release creation); their `.github` copies are gated to `workflow_dispatch` only until a Phase 2 pass rewrites that coupling for GitHub natively, and each carries a `PHASE-2 PORT PENDING` header comment plus a `GITEA_SYNC_TOKEN` secret reference in place of the old `GITEA_TOKEN` name.
