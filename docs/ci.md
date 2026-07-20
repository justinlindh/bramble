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
| `quality.yml` | The heavy firmware/emulator compute jobs: host tests, gosim, the board build smoke, and the emulator suite. |
| `webapp-quality.yml` | The consolidated webapp job (one `npm ci`, all webapp checks) plus the web-flasher tests. |

Publish-oriented workflows (`firmware-build.yml`, `firmware-publish-ota.yml`,
`ci-smoke-artifacts.yml`, `release-components.yml`, `webapp-build-publish.yml`,
`claude.yml`) are out of scope here; they run on `workflow_dispatch`, tags, or
their own events and do not gate normal PRs. Desktop installers (Linux,
Windows, macOS) are built and attached to webapp releases by
`release-components.yml` itself.

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
fires only on `main`, i.e. post-merge. Every gating job now also runs on
`pull_request`; there are no post-merge-only jobs left. There is no per-branch `push` trigger, so a
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
>
> MATRIX JOBS ARE THE EXCEPTION: they must gate on their STEPS, never on the
> job. A job-level `if:` that evaluates false stops GitHub from expanding the
> matrix at all, so instead of one context per leg it publishes a single check
> run carrying the raw literal job name, for example
> `Board build smoke (${{ matrix.board }})`. The per-leg contexts do not exist
> on that commit, so requiring them would block every out-of-scope PR forever.
> Gate each step instead: the matrix always expands, and out of scope each leg
> reports success having done no work.

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
| `firmware` | `main/`, `components/`, `test/`, `api/`, `scripts/`, `docker/firmware-builder/`, `CMakeLists.txt`, `sdkconfig.*`, `partitions*.csv`, `.clang-format`, `.clang-format-version`, `.clang-tidy`, `.shellcheckrc`, `.markdownlint-cli2.yaml` |
| `simulator` | `simulator/` |
| `emulator` | `emulator/` |
| `webapp` | `webapp/` |
| `web_flasher` | `web-flasher/` |
| `workflows` | any file under `.github/workflows/`, or `.actionlint.yaml` |
| `docs` | `docs/`, `README.md` |

`firmware` is deliberately a superset of every firmware-area consumer's real
inputs (host tests, board build, ruff, cppcheck, rpc-contract, and the strict
clang-format/shellcheck wrappers). `simulator` is a separate output
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
root `CMakeLists.txt`, `sdkconfig.defaults*`, and `partitions*.csv`. The firmware
area also covers `.releaserc.*` (root release configs) so the `Release config`
job's scope-gating regression runs on any release-config edit.

## Job topology

### `firmware-quality.yml`

| Job (context name) | Runs when | Required? |
| --- | --- | --- |
| `Detect changed areas` (via reusable `detect`) | always | no |
| `Static checks` | always (no `if:`) | yes |

`Static checks` is one pod, one checkout, and a sequence of named steps that each
preserve a former standalone job's exact command: no-internal-refs, the
board-matrix coverage check, strict shellcheck, ruff baseline, strict
clang-format, cppcheck, the rpc-contract check, and actionlint over the four
gating workflow files. Each check is its own step so
a failure attributes to the exact tool in the UI. It has no `if:` because it is
THE universal gate: it must run (and report) on every PR, including a docs-only
PR where it is the only job that executes. The individual checks are cheap and
pass trivially when their scope did not change.

The clang-format version is pinned, not just the binary's presence. The
`.clang-format-version` file at the repo root is the single source of truth
(currently `14.0.6`, matching what the runner image bakes); a dedicated step
runs `clang-format --version` and fails with a clear message naming both the
expected and actual version when they disagree, instead of letting a version
skew masquerade as a formatting violation (issue #161: two prior incidents
where a contributor's local clang-format and CI's disagreed on macro/designated-
initializer layout, with no text satisfying both). `run-clang-format-check.sh`
performs the same comparison locally and prints a warning banner (not a hard
failure, so a contributor without the exact version can still get advisory
signal) naming the mismatch and pointing at this file.

### `quality.yml`

| Job (context name) | Runs when | Required? |
| --- | --- | --- |
| `Detect changed areas` (via reusable `detect`) | always | no |
| `Host tests` | `firmware` or `workflows` | yes |
| `Parser fuzzing` | `firmware` or `workflows` | yes |
| `Release config` | `firmware` or `workflows` | yes |
| `gosim integration` | `firmware`, `simulator`, or `workflows` | yes |
| `Board build smoke (heltec-v3)` | `firmware` or `workflows` | yes |
| `Board build smoke (tdeck-plus)` | `firmware` or `workflows` | yes |
| `Board build smoke (heltec-v4)` | `firmware` or `workflows` | yes |
| `Board build smoke (bramble-pager)` | `firmware` or `workflows` | yes |
| `Emulator suite` | `firmware`, `simulator`, `emulator`, or `workflows` | yes |

`Parser fuzzing` runs `test/fuzz/run_fuzz.sh`, a bounded libFuzzer campaign
(30 seconds per target, two targets) under ASan and UBSan against the wire-frame
parsers in `components/packet/packet.c` and the fragment reassembler in
`components/fragment/fragment.c`. Those parsers run on attacker-controlled LoRa
bytes before any AEAD tag or HMAC is verified, which is what earns them a
dedicated gate. The seed corpora are committed under `test/fuzz/corpus/`, so the
run starts from valid frames of every wire type and the budget is spent on
mutation rather than on rediscovering the formats. The job needs `clang` with
the libFuzzer and sanitizer runtimes on the runner image; a first step asserts
the toolchain is present and fails the job when it is not, because skipping on a
missing tool would make the check advisory. A finding uploads its reproducing
input as a CI artifact, and the same input replays locally with
`test/fuzz/build/fuzz_packet <file>`.

`Emulator suite` merges the former `emulator-scenarios` and `emulator-e2e`
jobs into one job that builds the linux firmware node, gosim, and the UI once,
then runs the headless scenario suite followed by the browser E2E. Both suites
gate the job via a collect-then-fail pattern: the two suite steps carry
`continue-on-error: true` purely as an error-collection mechanism (so one run
always reports both suites' results, even when the first fails), and the
terminal `Fail if any suite failed` step turns either step's failure into the
job failure the required context sees. There is no advisory tier: every check
in the job gates.

The two scenarios run sequentially inside `emulator/ci/run_scenarios.sh`: they
are isolated (each gosim process keys its socket path and node-state dir to its
own PID, so there is no port or state collision), but running both at once put
five wall-clock firmware nodes on the runner simultaneously and the CPU
contention starved their real-time windows, which was a flake source.
`emu-dm-desync` is deterministic (the desynced session state is constructed,
not raced) and runs exactly once, with no retries of any kind. A node process
death mid-scenario is a hard suite failure in BOTH scenarios, checked on
every run including passing ones (a crashed-and-restarted node can still
satisfy a render assertion, and any rerun would absorb intermittent crash
regressions); the death evidence dumps into the step log and the scenario
logs upload as a CI artifact on failure. `emu-channel-delivery` runs once with an event-driven render
wait: the script widens gosim's real-time cap (`EMU_SCENARIO_DURATION_MS`) and
polls the growing headless log for the render marker, exiting the moment both
receivers have painted, so a fast box finishes in seconds and a CPU-starved
runner pod gets the time it needs; there is no retry loop. Failed scenarios
dump a post-mortem (attach/join/death events, per-node framebuffer counts, log
tail) so pod-only failures stay debuggable. Note the emulator's scheduling
fidelity caveat (emulator/README.md): linux-target task priorities are
flattened for liveness, so genuine priority-starvation bugs are hardware-only
detection; this suite cannot catch them.

`Release config` runs the release-rule scope-gating regression
(`node scripts/release/semantic-release-squash-expander.test.cjs`) after a small
`npm install` of the pinned `@semantic-release/commit-analyzer`,
`release-notes-generator`, and `conventional-changelog-conventionalcommits`. It
loads the real `.releaserc.<component>.cjs` files and asserts, per component,
that an in-scope `fix`/`feat`/`perf`/breaking commit cuts the right release
level while out-of-scope and non-releasing commits do not, so a scope-gating
regression that would silently stop every binary from publishing fails the PR.

`Board build smoke` is a four-way matrix over `heltec-v3`, `tdeck-plus`,
`heltec-v4`, and `bramble-pager`: the same four targets `scripts/ci-build-firmware.sh`
builds for a release. It used to build only `heltec-v3` and to carry a
`github.event_name != 'pull_request'` guard, which meant three of the four
shipped boards were never built by any automatic run. The targets diverge on
display stack (ST7789/LVGL vs SSD1680 e-paper vs SSD1306), touch, keyboard, and
trackball, so a change under `components/display` or `components/ui_graphics`
could break three of four with every required context reporting green until
someone manually dispatched a release build.

Both halves of that are now gone: the matrix covers all four boards and the
`pull_request` guard is dropped, so the board builds gate PRs like every other
required check. There is no longer a non-PR required check. The strictness
jump is affordable because ccache landed first (see "Build caching" below);
without a compiler cache, four cold ESP-IDF builds per PR would not have been.

The matrix sets `fail-fast: false` so every board reports its own result: with
fail-fast on, the first failure cancels its siblings and hides whether a break
is one board or all four, which is the exact information the matrix exists to
produce. `max-parallel: 2` keeps the small self-hosted pool from being fully
occupied by board builds while the host tests, gosim, and emulator jobs are
waiting for a pod; a queued job does not burn its own timeout.

Job name templating (`Board build smoke (${{ matrix.board }})`) makes the
heltec-v3 context string identical to the one the un-matrixed job produced, so
that required check carries over untouched. The other three are new required
context names and have to be added to branch protection.

The area gate lives on the job's STEPS, not on the job, and it has to stay
there. This is the one place in the repo where the usual job-level `if:` is
wrong. With a job-level `if:` the matrix only expanded when the condition was
true; when it was false GitHub published a single check run named with the
unevaluated literal `Board build smoke (${{ matrix.board }})` and the four
per-board contexts simply did not exist on that commit. Requiring them would
then have hung every docs-only, webapp-only, and dependency PR forever.
Gating each step keeps the matrix unconditional, so all four contexts report
on every PR: they build when `firmware` or `workflows` changed, and otherwise
succeed immediately having run nothing. Verified by opening a docs-only PR
against this change and observing all four expanded contexts report success.

`scripts/lint/check-board-matrix.sh` (a step in the `Static checks` bundle)
asserts that the matrix board list and the `BOARDS` list in
`scripts/ci-build-firmware.sh` are identical, so adding a fifth board to the
release path without adding it to the gate fails the PR rather than quietly
reopening the hole.

`Docker build (webapp)`, `(simulator)`, `(firmware-builder)` (issue #195) is
a matrix, one leg per covered Dockerfile. Before this job, no workflow built
any of the shipped Dockerfiles: a base-image bump (the trigger case, #179's
`node` 22-to-26 jump) could merge with every required context green and
nothing having ever constructed the image, which is worse than no bump at
all because it carries the appearance of a passing check. Each leg runs
`docker build` against its own Dockerfile and build context, directly on the
runner host, exactly as every other job in this workflow runs its own
toolchain (`idf.py`, `go`, `npm`) directly rather than inside a `container:`.
Nothing is pushed or loaded, since the job exists to catch a broken
Dockerfile before merge; a successful build is the entire assertion.
`DOCKER_BUILDKIT=1` is forced on so the Dockerfiles' `# syntax=` directives
and `--mount=type=cache` RUN steps are honoured by the host daemon's
BuildKit. No layer-cache backend is wired yet: a registry or `type=gha`
cache needs the buildx docker-container driver plus the runner's cache
service, both extra failure surface, so caching is a follow-up once the
plain build is proven green on the pool.

`emulator/Dockerfile` is the fourth shipped Dockerfile but is deliberately
NOT a leg yet. Its ESP-IDF stage resolves managed components (libsodium,
lvgl) from the Espressif component registry at build time, which needs live
egress to `components-file.espressif.com` that the runner pool does not have,
so the image cannot build from a clean checkout on CI. The gate caught this
as a real latent bug: the emulator image had never been buildable from clean
(the `Emulator suite` builds `emulator/node` directly on the host, never
through the Dockerfile, and a gitignored `dependencies.lock` masks it
locally). Making that build hermetic (vendored or baked components, no live
registry egress) and re-adding the leg is tracked in issue #231; the PR that
added this gate also fixed a separate latent bug where the manager targeted
the dead `api.components.espressif.com` subdomain, so `docker compose up
--build` works for developers with normal egress even though the CI leg is
deferred.

The area gate is on the job's STEPS, not the job, for the same
matrix-collapse reason as `Board build smoke` above. Each leg's gate is the
union of areas that Dockerfile's `COPY` instructions actually read: `webapp`
gates on `webapp`; `simulator` (which also `COPY`s `main/`, `components/`,
and `test/stubs/` for its cgo build) gates on `simulator` or `firmware`;
`firmware-builder` gates on `firmware`, which now includes
`docker/firmware-builder/` itself (see the detector's area table above).
`workflows` is OR'd into every leg, per the usual safety rule.
`max-parallel: 2` for the same small-pool reason as the board matrix.

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

## Build caching: ccache for the ESP-IDF compiles

Two kinds of job in `quality.yml` run a full ESP-IDF compile: the four
`Board build smoke` matrix legs and the linux firmware node build inside
`Emulator suite`. Both enable ESP-IDF's
native ccache support through `scripts/ci-ccache-env.sh`, which exports
`IDF_CCACHE_ENABLE=1` (the variable `idf.py` reads to put ccache in front of
the cross compiler) plus a `CCACHE_DIR` under `RUNNER_TEMP`. The build steps
override `HOME` to a scratch path, so ccache's default `~/.ccache` would move
between steps and never be cacheable; pinning the directory outside both the
workspace and `HOME` is what makes it survive.

The cache directory itself is persisted with `actions/cache`, not with a
directory on the runner host. The self-hosted pool is an ARC scale set whose
runner pods are ephemeral: the pod filesystem is destroyed after each job, and
a workflow cannot mount a host path or a volume into it. `actions/cache` is
the only persistence mechanism available to the workflow, and it works from
self-hosted runners because it talks to the GitHub cache service rather than
to local disk.

There is deliberately no `actions/cache` entry for `~/.espressif`. ESP-IDF and
its tool downloads are baked into the runner image (`scripts/ci-ensure-idf.sh`
asserts they are present and never installs at runtime), so caching the tools
directory would cache something that is already local to every pod.

Cache keys are `ccache-<target>-idf<version>-<sdkconfig hash>-<run id>`, where
`<target>` is the matrix board (so each board keeps its own cache), with
prefix `restore-keys` falling back to the newest entry for the same ESP-IDF
version and sdkconfig. The ESP-IDF version is in the key because objects built
by one toolchain must never be served to another; the sdkconfig hash is in it
because a config change invalidates most of the build. The run-id suffix makes
every key unique, which is what lets a run save an updated cache (GitHub cache
entries are immutable once written) while still restoring the previous one.

ccache is an accelerator, not a gate. If the runner image does not ship
`ccache`, `scripts/ci-ccache-env.sh` logs a notice, reports `available=false`,
and the build runs uncached; the cache and stats steps are skipped by that
output. A missing accelerator must not fail a correctness check.

## Dual-tree arrangement: .github is authoritative, .gitea is a frozen mirror

`.github/workflows/` is the source of truth for CI and runs on the self-hosted
ARC scale set (runner label from the `RUNNER_LABEL` repo variable).
`.gitea/workflows/` is a frozen, unmodified mirror-side copy and is not touched
by workflow edits. The publish-oriented workflows still carry Gitea API coupling
and are gated to `workflow_dispatch` until a Phase 2 pass rewrites them for
GitHub natively; each carries a `PHASE-2 PORT PENDING` header.
