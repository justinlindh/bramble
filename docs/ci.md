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

Publish-oriented workflows (`firmware-build.yml`, `release-components.yml`,
`webapp-build-publish.yml`, `claude.yml`) plus the dispatch-only runner health
check `ci-smoke-artifacts.yml` are out of scope here; they run on
`workflow_dispatch`, tags, or their own events and do not gate normal PRs. Desktop installers (Linux,
Windows, macOS) are built and attached to webapp releases by
`release-components.yml` itself. Firmware OTA is published only by a manual
dispatch of `firmware-build.yml`, never by merging to `main`; see "Firmware OTA
publishing is manual" below before you assume a merged firmware release reached
any device.

Two housekeeping workflows (`burst-runner-watchdog.yml`, `cache-cleanup.yml`)
also sit outside the gating set: they keep CI's shared resources healthy,
report no context, and gate nothing. See
[Cache budget hygiene](#cache-budget-hygiene) for what the second one does and
why it has to exist.

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
fires only on `main`, i.e. post-merge. Every gating job also runs on
`pull_request`; there are no post-merge-only jobs. There is no per-branch
`push` trigger: with one, a PR would get two overlapping waves (a
`pull_request` wave and a `push`-to-`fix/**` wave) running to completion in
separate concurrency groups, doubling the queue.

Each workflow sets `concurrency` keyed on the ref with
`cancel-in-progress: ${{ github.event_name == 'pull_request' }}`, so a new push
to a PR cancels the superseded PR run but never cancels a `main` run.

### Fork pull requests never touch the self-hosted pool

The repo is public, so fork-origin work is fenced twice.

First, the repository Actions setting "Require approval for all external
contributors" (REST:
`repos/{owner}/{repo}/actions/permissions/fork-pr-contributor-approval`,
policy `all_external_contributors`) holds every workflow run on a fork-head PR
until a maintainer approves it, on every push, regardless of the author's
contribution history. The weaker default (`first_time_contributors`) would
grant permanent unapproved runs to anyone with a single merged commit, which
is the wrong trade for self-hosted runners. This setting lives in
repository configuration, not in this tree; treat this paragraph as the
contract and restore the policy if it ever drifts.

Second, every job that targets the pool selects its runner with

```yaml
runs-on: ${{ github.event.pull_request.head.repo.fork && 'ubuntu-latest' || vars.RUNNER_LABEL || 'ubuntu-latest' }}
```

so an approved fork-head run executes on GitHub-hosted runners and fork code
never reaches the pool; for same-repo PRs, `main` pushes, and dispatches the
fork test is false or absent, so they follow `RUNNER_LABEL`.
The expression is defense-in-depth, not the boundary: a `pull_request` run
takes its workflow definitions from the PR's merge ref, so a fork PR can edit
the expression out. The approval gate above is what stands between an
untrusted author and any run at all, and reviewing the diff (including
workflow edits) before approving is what keeps a tampered `runs-on` off the
pool. The workflows that hold write tokens (`claude.yml`, `cache-cleanup.yml`)
are author-association-gated or run no repository code, and both are pinned to
GitHub-hosted runners; their in-file comments carry the details.

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
to live inline in each workflow with one definition. It runs once per caller
that actually reads it, which is two detector pods per PR wave
(`quality.yml` and `webapp-quality.yml`), and the diff-to-area mapping lives in
exactly one place.

Call it only from a workflow that gates on its outputs. `firmware-quality.yml`
used to call it and never read a single output: its one job has no `if:`, so
there was nothing to gate. That cost a pod on a full-history checkout per wave,
and because the call was wired with `needs:`, it also delayed the universal
gate, the one job that runs on a docs-only PR, behind a job whose results
nobody read.

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
| `firmware` | `main/`, `components/`, `CMakeLists.txt`, `sdkconfig.*`, `partitions*.csv` |
| `host_test` | `test/` |
| `simulator` | `simulator/` |
| `emulator` | `emulator/` |
| `webapp` | `webapp/` |
| `web_flasher` | `web-flasher/` |
| `api` | `api/` |
| `idf_build_scripts` | `scripts/ci-source-idf*`, `scripts/ci-ensure-idf*`, `scripts/ci-ccache-env*`, `scripts/flash*`, `scripts/flash-all*`, `scripts/ensure-ota-signing-key*`, `.esp-idf-version` |
| `release_config` | `.releaserc.*`, `scripts/release/`, root `package.json` / `package-lock.json` |
| `ota_index` | `scripts/build-firmware-index.js`, `scripts/validate-firmware-index.js`, `test/test-validate-firmware-index.js`, `test/fixtures/firmware-index-*.json` |
| `nrf` | `nrf/`, `.arm-gcc-version` |
| `docker_firmware_builder` | `docker/firmware-builder/` |
| `coverage_tooling` | `scripts/ci/check_coverage.py`, `scripts/ci/host_coverage.py`, `scripts/ci/run-host-coverage.sh`, `ci/coverage-baseline.json` |
| `size_tooling` | `scripts/ci/check-firmware-size.sh`, `scripts/ci/check_size.py`, `scripts/ci/extract_firmware_sizes.py`, `ci/size-baseline.json` |
| `ci_core` | a job-defining gating workflow: `quality.yml`, `firmware-quality.yml`, `webapp-quality.yml`, or `_detect-changes.yml` (forces every heavy job) |

`firmware` is the compiled-firmware source set shared by every firmware BUILD
job (host tests, gosim, emulator, board build): the C under `main/` +
`components/` plus the root build config those builds read. It deliberately does
NOT carry `scripts/`, `api/`, `.releaserc.*`, `docker/firmware-builder/`, or the
lint dotfiles (`.clang-format`, `.clang-format-version`, `.clang-tidy`,
`.shellcheckrc`, `.markdownlint-cli2.yaml`): those are consumed only by the
always-run `Static checks` bundle (ruff scans `scripts/`, the rpc-contract check
reads `api/` + `main/`, the strict clang-format/shellcheck/markdownlint wrappers
read the root dotfiles), by the single-purpose `Release config` job, or by the
firmware-builder Docker leg, none of which the host tests, gosim, or emulator
suite ever read. Folding them into `firmware` used to spin up those heavy
firmware suites on scripts-only, api-only, dotfile-only, and release-config-only
PRs for no reason. `api` exists because the spec has a consumer OUTSIDE the
always-run bundle: `webapp/test/rpcContractCoverage.test.ts` pins the committed
`src/types/rpcContract.generated.ts` against `api/openapi.yaml`, so an api-only
spec edit gates the `Webapp checks` job (the rpc-contract check in `Static
checks` covers only the firmware side). `host_test` is `test/` on its own so a
host-harness-only change runs `Host tests` and `Parser fuzzing` without
dragging in gosim or the emulator suite. `simulator` is a separate output
because the strict clang-format scope scans simulator C sources too, so a
simulator-only change must still run clang-format (inside the bundle) and
gosim. `idf_build_scripts` is a small deliberate superset: the exact ESP-IDF
sourcing / flash / ccache wrapper scripts the emulator and board-build jobs
invoke (`ci-ccache-env.sh` included: both jobs run it to configure the compiler
cache, so editing it must re-run them), plus the `.esp-idf-version` pin, keyed
as one bucket (a `flash.sh`-only change re-running the emulator is a harmless
over-match, and over-matching here is always safe). `release_config` is exactly
what the `Release config` job's scope-gating step loads; `ota_index` (the OTA
firmware-index generator, validator, their schema fixtures, and the validator
test) gates that job's separate validator step, kept out of `release_config` so
an index-tooling change does not drag in the release job's `npm ci`.
`docker_firmware_builder` is the
firmware-builder image's build context (`docker/firmware-builder/`): that leg
builds a toolchain image FROM `espressif/idf` and does not `COPY` `main/` or
`components/`, so it gets its own area instead of riding on `firmware`.
`coverage_tooling` and `size_tooling` are the CI ratchet scripts and their
baselines under `scripts/ci/` and `ci/`: when the broad `firmware` area still
carried `scripts/`, editing a ratchet rode along on every firmware change; with
`firmware` narrowed, these areas re-run exactly the jobs whose steps execute the
ratchet (host tests, gosim, and the webapp job for coverage; the board build for
size) so a ratchet-script-only or baseline-only change still self-verifies.
`nrf` is the bare-metal nRF52840 target tree (`nrf/`): its build job compiles
`components/` sources directly, so it fires on `firmware` too, but a
`nrf/`-only change (its shims, configs, or scripts) runs just that job plus the
always-on bundle. The `.arm-gcc-version` pin sits in this area despite living at
the repo root, because it names the compiler the job's memory gate measures
with: changing it changes every byte count that gate enforces, so it has to
re-run the build that enforces them.

### The safety rule: job-defining workflow edits force everything

`ci_core` is OR'd into every heavy job's `if:`. It matches ONLY the workflows
that DEFINE the build/test jobs: `quality.yml`, `firmware-quality.yml`,
`webapp-quality.yml`, and the reusable `_detect-changes.yml` they all call.
Editing one of those can change a job's steps or the gating logic itself, and
that change must be exercised: if a job-defining edit only ran jobs whose code
area also happened to change, a broken job definition could land unverified. So
editing any of those four runs every heavy job across all three workflows. This
over-triggers on rare core-workflow edits on purpose, in exchange for a rule
that is impossible to under-match.

Editing any OTHER workflow file does NOT force the product suites, because it
cannot affect what they build. The `claude.yml` review bot, `commit-msg-lint.yml`,
`pr-template.yml`, the publish/release workflows (`firmware-build.yml`,
`release-components.yml`, `webapp-build-publish.yml`, ...), the burst-runner
watchdog, and the Dependabot config each run their own jobs via their own
triggers; a PR that touches only them runs just the always-on `Static checks`
bundle here (plus that workflow's own jobs), while every heavy context still
reports a green skip so branch protection is never stranded. `.actionlint.yaml`
is likewise not in `ci_core`: it only feeds the actionlint step inside `Static
checks`, which always runs. This is what stops a review-bot-config or markdown
edit from spinning up the full emulator / board / docker / host / gosim suite.

### The area superset rule

Every job's area set must be a superset of what its commands actually read. When
a script widens its scan scope, widen the matching area regex or job condition
with it, or PRs touching only the new scope will falsely skip the check. Audit
by reading the scripts, not the job names. Because the `Static checks` bundle has
no `if:` and always runs, the widest-scanning strict wrappers
(`run-clang-format-check.sh --strict` over `main/ components/ test/ simulator/`,
`run-shellcheck.sh --strict` over `scripts/lint/*.sh`, cppcheck over `main
components`, ruff over `scripts/`, and the rpc-contract check over `api/` +
`main/`) never need an area at all: they gate on every PR regardless. The
per-area outputs exist only for the HEAVY jobs, so each area is sized to exactly
what its job builds: the host test suite builds `test/` (`host_test`) against
`components/` + `main/` (`firmware`) and ratchets host coverage
(`coverage_tooling`); `Parser fuzzing` builds `test/fuzz/` (`host_test`) against
`components/` (`firmware`); gosim builds `simulator/` against `components/` +
`main/` and ratchets gosim coverage (`coverage_tooling`); the emulator suite
builds `emulator/` + `simulator/` + `components/` + `main/` and sources the IDF
wrapper scripts (`idf_build_scripts`); the four-board build (which gates PRs)
additionally reads root `CMakeLists.txt`, `sdkconfig.defaults*`,
`partitions*.csv` (`firmware`), its flash wrappers (`idf_build_scripts`), and the
size ratchet (`size_tooling`); the `Release config` job loads `.releaserc.*` +
`scripts/release/` and installs the root `package.json` / `package-lock.json`
(`release_config`); and the firmware-builder Docker leg builds
`docker/firmware-builder/` (`docker_firmware_builder`). If one of these jobs
gains a new input, widen its area, not the shared `firmware` catch-all.

## Job topology

### `firmware-quality.yml`

| Job (context name) | Runs when | Required? |
| --- | --- | --- |
| `Static checks` | always (no `if:`) | yes |

One job, and no `detect` call: `Static checks` has no `if:`, so it has nothing
to gate on. See [The reusable detector](#the-reusable-detector) for why calling
the detector here was worse than merely useless.

`Static checks` is one pod, one checkout, and a sequence of named steps that each
preserve a former standalone job's exact command: no-internal-refs, no-em-dash,
markdownlint (issue #160), the board-matrix coverage check, strict shellcheck,
ruff baseline, strict clang-format, cppcheck, the rpc-contract check, and
actionlint over every workflow file. Each check is its own step so
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

The ARM cross-compiler is pinned the same way and for a sharper reason.
`.arm-gcc-version` at the repo root is the single source of truth for the
compiler the nRF52840 target builds with; `scripts/lint/check-arm-gcc-version.sh`
gates every reference against it, and both the `nRF52840 build` job's assert
step and `nrf/CMakeLists.txt` read that file rather than restating the version.
The pin matters because that job's memory gate fails the build on a byte
count, and a byte count is a property of the compiler as much as of the source:
across GCC releases the same commit differs by more than the headroom the
T1000-E build runs at. Without the pin, a developer's distro compiler produces a
size number in the same format as CI's, against a different budget consumption,
and the two get compared as though they were one measurement. Configuring the
nRF build with an unpinned compiler is a loud CMake warning rather than an error,
because a contributor cannot always install an exact GCC from their distro;
`nrf/scripts/size_report.py` then stamps the compiler into its verdict line and
its JSON, so a mismatched number is never mistaken for CI's.

The version pin narrows the gap without closing it. A toolchain ships a C
library with the compiler, and newlib's own statics differ between distributions
of the same GCC release: Arm's `13.2.Rel1` binary release reports the pinned
`13.2.1` and still lands 144 bytes below the runner image's count for the
T1000-E. Only CI's exact packaging reproduces CI's bytes, so `nrf/README.md`
documents an `ubuntu:24.04` container that installs the same
`gcc-arm-none-eabi` package the image bakes, and that is the answer for a
question that turns on bytes. Where the pin sits is a judgement, not an
accident: `13.2.1` is what the runner image bakes from Ubuntu Noble and what
every nRF budget margin is measured against, and across the three toolchains
measured for this gate the T1000-E static figure spans 106336 to 106488 bytes
against a 106496 ceiling. Which toolchain builds is therefore worth more than
the margin the gate has left, which is what makes a move to a newer GCC a
deliberate change rather than a version bump: it edits `.arm-gcc-version`,
bumps `ARM_GCC_VERSION` in the private runner-image definition so the image
agrees, updates every file `check-arm-gcc-version.sh` names, and re-measures
both boards' budgets in the same change. The version pin is still what keeps the
runner image, the workflow and the docs from drifting apart, and what keeps a local
build in the same GCC generation as CI.

### `quality.yml`

| Job (context name) | Runs when | Required? |
| --- | --- | --- |
| `Detect changed areas` (via reusable `detect`) | always | no |
| `Host tests` | `firmware`, `host_test`, `coverage_tooling`, or `ci_core` | yes |
| `Parser fuzzing` | `firmware`, `host_test`, or `ci_core` | yes |
| `Release config` (step-gated) | `release_config`, `ota_index`, or `ci_core` | yes |
| `nRF52840 build` | `firmware`, `nrf`, or `ci_core` | yes |
| `gosim integration` | `firmware`, `simulator`, `coverage_tooling`, or `ci_core` | yes |
| `Board build smoke (heltec-v3)` (+ `tdeck-plus`, `heltec-v4`, `bramble-pager`), step-gated | `firmware`, `idf_build_scripts`, `size_tooling`, or `ci_core` | yes |
| `Emulator suite` | `firmware`, `simulator`, `emulator`, `idf_build_scripts`, or `ci_core` | yes |
| `Docker build (webapp)`, step-gated | `webapp`, `web_flasher`, or `ci_core` | yes |
| `Docker build (simulator)`, step-gated | `simulator`, `firmware`, or `ci_core` | yes |
| `Docker build (emulator)`, step-gated | `emulator`, `simulator`, `firmware`, or `ci_core` | yes |
| `Docker build (firmware-builder)`, step-gated | `docker_firmware_builder` or `ci_core` | yes |

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
(`node scripts/release/semantic-release-squash-expander.test.cjs`) after an
`npm ci` from the root `package-lock.json`, which is the same lockfile
`release-components.yml` installs, so the test runs against the exact
semantic-release toolchain that cuts releases. It
loads the real `.releaserc.<component>.cjs` files and asserts, per component,
that an in-scope `fix`/`feat`/`perf`/breaking commit cuts the right release
level while out-of-scope and non-releasing commits do not, so a scope-gating
regression that would silently stop every binary from publishing fails the PR.

The same job hosts a second, independent release/publish node test: the OTA
firmware-index validator (`node test/test-validate-firmware-index.js`), gated on
its own `ota_index` area. It pins `scripts/validate-firmware-index.js` to
`docs/ota-release-schema.md` (the validator `scripts/publish-firmware-release.sh`
runs on the `index.json` it generates), so a schema regression that would ship an
index the OTA journey cannot consume fails the PR. The two tests share the pod
because both need only node, but each is gated on its OWN area at the STEP level:
an `ota_index`-only change runs the validator step and skips the `npm ci` +
scope-gating steps, a `release_config`-only change does the reverse, and
`ci_core` forces both. The `Release config` context name is unchanged, so branch
protection needs no new required context.

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
on every PR: they build when `firmware`, `idf_build_scripts`, `size_tooling`, or
`ci_core` changed, and otherwise succeed immediately having run nothing. Verified
by opening a docs-only PR against this change and observing all four expanded
contexts report success.

`scripts/lint/check-board-matrix.sh` (a step in the `Static checks` bundle)
asserts that the matrix board list and the `BOARDS` list in
`scripts/ci-build-firmware.sh` are identical, so adding a fifth board to the
release path without adding it to the gate fails the PR rather than quietly
reopening the hole.

`Docker build (webapp)`, `(simulator)`, `(emulator)`, `(firmware-builder)`
(issue #195) is a four-way matrix, one leg per shipped Dockerfile. Before this
job, no workflow built any of them: a base-image bump (the trigger case,
\#179's `node` 22-to-26 jump) could merge with every required context green and
nothing having ever constructed the image, which is worse than no bump at
all because it carries the appearance of a passing check. Each leg builds its
own Dockerfile and build context with buildx; nothing is pushed or loaded,
since the job exists to catch a broken Dockerfile before merge, and a
successful build is the entire assertion.

The legs run on GitHub-hosted `ubuntu-latest` runners, not the self-hosted
pool, because the caching model requires it. The pool provides docker to each
runner pod through an ephemeral dind sidecar whose daemon state is destroyed
with the pod, so every build there was fully cold: neither the layer cache nor
the Dockerfiles' `--mount=type=cache` mounts survived a single job, and
multi-GB image builds competed with the firmware suites for pool pods and pool
disk I/O. Hosted runners ship docker and buildx ready for the GitHub cache
service, so each leg round-trips its layer cache with `type=gha` under a
per-image scope (`docker-webapp`, `docker-simulator`, ...), `mode=max` so
intermediate multi-stage layers are cached too, which is where the toolchain
and dependency-install cost lives. Cache entries follow the same branch
scoping as `actions/cache`: a PR restores from its own branch or from `main`,
and the post-merge `push` run on `main` is what publishes the shared warm
cache. The docker scopes share the repository's 10 GB Actions cache quota
with the ccache entries; eviction is LRU and only costs a colder build, never
a failure.

The Dockerfiles are ordered for that cache: every stage installs its
toolchain and dependencies (keyed on lockfiles or manifests alone) before any
source COPY, so an ordinary code change re-runs only the final compile
layers. The root `.dockerignore` whitelists exactly the trees the
root-context Dockerfiles COPY, which keeps the context upload small (no
`.git`, no `node_modules`, no local build output) and stops gitignored local
artifacts (`emulator/node/build/`, `sdkconfig`, `dependencies.lock`, ...)
from busting or poisoning layers, so a local build is byte-identical to
CI's. `emulator/Dockerfile` builds its ESP-IDF stage `FROM
espressif/idf:v5.4.1` (the same pinned base `docker/firmware-builder/
Dockerfile` uses) instead of git-cloning esp-idf and running `install.sh`
inside the build, which was the bulk of that image's cold build time.

The gate caught a real latent bug on its first run: `emulator/Dockerfile`'s
ESP-IDF stage resolves managed components (libsodium, lvgl) from the Espressif
component registry at build time, and the manager bundled with v5.4.1
defaulted to the now-dead `api.components.espressif.com` subdomain, so the
emulator image had never been buildable from a clean checkout (the
`Emulator suite` builds `emulator/node` directly on the host, never through
the Dockerfile, and a gitignored `dependencies.lock` masked it locally). The
registry-URL pin in `emulator/Dockerfile` fixes that for CI and for
`docker compose up --build` alike, and the root `.dockerignore` excludes
`dependencies.lock` so local builds resolve exactly as CI does. That live
fetch is consistent with the rest of CI: the required `Emulator suite` builds
`emulator/node` by fetching the same components live, and the other docker
legs run live `npm ci` / `go mod download` / `apt` at build time. Hosted
runners reach `components-file.espressif.com` from the default build network,
which retires the `--network=host` workaround the emulator leg needed on the
pool (the component file host was not reachable from the pods' default bridge
network).

The area gate is on the job's STEPS, not the job, for the same
matrix-collapse reason as `Board build smoke` above. Each leg's gate is the
union of areas that Dockerfile's `COPY` instructions actually read: `webapp`
gates on `webapp` or `web_flasher` (the leg stages the top-level `web-flasher/`
tree into `webapp/public/web-flasher/` before building, exactly as the publish
workflow does, so the image it verifies contains web-flasher and a
web-flasher-only change must rebuild it); `simulator` (which also `COPY`s
`main/`, `components/`, and `test/stubs/` for its cgo build) gates on
`simulator` or `firmware`;
`emulator` (which additionally builds the linux firmware node and gosim)
gates on `emulator`, `simulator`, or `firmware`; `firmware-builder` gates on
`docker_firmware_builder` alone, because its build context is
`docker/firmware-builder/` (a toolchain image that does not `COPY` `main/` or
`components/`), so a firmware source change must not rebuild it. `ci_core` is
OR'd into every leg, per the usual safety rule. There is no `max-parallel`
cap: the legs run on isolated hosted VMs, so they cannot crowd out the
self-hosted pool.

### `webapp-quality.yml`

| Job (context name) | Runs when | Required? |
| --- | --- | --- |
| `Detect changed areas` (via reusable `detect`) | always | no |
| `Webapp checks` | `webapp`, `api`, `coverage_tooling`, or `ci_core` | yes |
| `web-flasher tests` | `web_flasher` or `ci_core` | yes |

`Webapp checks` is one job with a single `npm ci`, then typecheck, electron
typecheck, unit tests, build, and the e2e smoke run as sequential steps. The smoke
run reuses the build produced earlier in the same job, so there is no second
`npm ci` and no second build, and `tsc --noEmit` runs exactly once.

That last part was not true until recently, and the reason is a trap worth
naming: `webapp/package.json` defines `lint` as a bare alias for `typecheck`,
and `build` as `npm run typecheck && vite build`. A job with separate Lint,
Typecheck, and Build steps therefore ran the identical `tsc --noEmit` three
times. The build step now calls `vite` directly (verified to emit a
byte-identical `dist/`), leaving the one explicit Typecheck step, which is
where a type failure should attribute anyway.

There is deliberately no lint step. The webapp has no linter: no eslint,
biome, or prettier config, and no such dependency. The former Lint step ran
the typecheck alias and was removed rather than renamed, because a step named
for a gate that does not exist is worse than no step. Adding a real linter is
an open decision, not a rename, and this page will say so only once one
exists. `web-flasher tests` stays separate because it runs
`node --test 'web-flasher/**/*.test.js'` with no webapp install.

## What a docs-only (or review-bot-config) PR looks like now

A PR that touches only `docs/**` still triggers all three workflows. The two
`detect` jobs run (fast: a diff, no build), the `Static checks` bundle runs (one
pod, and starts immediately rather than waiting on a detector it does not read),
and every other job reports `skipped`. The same holds for a PR that edits
only a NON-core workflow plus a doc, e.g. `CLAUDE.md` + `.github/workflows/claude.yml`
(the review bot): because `claude.yml` is not in `ci_core`, none of the heavy
firmware / emulator / board / docker / host / gosim / webapp suites activate, and
`claude.yml` runs its own jobs via its own triggers. Every context branch
protection could require gets a report either way (green skip where inactive), so
the PR is never stuck waiting on a check that was never going to run.

## Adding a new job

1. Add `needs: detect`.
2. Add an `if:` referencing the relevant `needs.detect.outputs.*` area(s), OR'd
   with `ci_core` at minimum (so editing a job-defining workflow re-exercises the
   job). (The `Static checks` bundle is the sole deliberate exception: no `if:`,
   so it always runs and reports.)
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
output. The `Restore ccache` steps additionally carry
`continue-on-error: true`: `actions/cache` can hard-fail on a cache-service
outage or a DNS blip after its internal retries (observed 2026-07-24, a
transient `EAI_AGAIN` failed a board leg), and a restore failure must cost a
cold build, never a red check. This is the accelerator exception to the
no-advisory-tier rule, not a softened check: the step produces no
correctness signal, nothing downstream reads its outcome, so no terminal
fail-collector step applies. A missing accelerator must not fail a
correctness check.

## Cache budget hygiene

Everything above that makes CI fast draws on one shared, finite resource: the
repository's 10 GB GitHub Actions cache budget. The ccache entries described in
the previous section, the `buildkit-blob` and `index-docker-*` layer cache the
four `Docker build` legs write with `type=gha,mode=max`, and anything code
scanning stores all come out of the same 10 GB.

Cache entries are scoped to the ref that wrote them. A pull request's entries
live under `refs/pull/<n>/merge` and can only ever be restored by that same
pull request, and GitHub does not delete them when the pull request closes.
Nothing here deleted them either, so they accumulated. Measured before
`cache-cleanup.yml` existed: 10.15 GB used of 10 GB across 449 entries, 2.9 GB
of it (29 percent) belonging to twelve pull requests that had all already
merged. Over budget, GitHub frees space by evicting the least recently used
entry, so those dead entries were not merely idle, they were displacing live
ones. A cold Docker leg costs about five minutes against about 25 seconds warm,
and a cold board build gives up everything ccache is there for, so the failure
mode was not "no caching" but "caching that works until it randomly does not",
which is considerably harder to notice.

`cache-cleanup.yml` reclaims that space. It purges a pull request's caches on
`pull_request_target: [closed]`, and a daily sweep re-derives the whole dead
set from the live cache listing and each pull request's current state. The
sweep is not a backstop for a broken primary path, it is the reconciliation
half of the design: a run still in flight when its pull request merges writes
its cache after the close event, and no event-driven purge can catch that.

Two things about it are deliberate. It uses `pull_request_target` rather than
`pull_request` because deleting a cache entry needs `actions: write`, which a
fork's `pull_request` token does not grant; it is safe here only because the
workflow never checks out or executes repository code, and that property has to
hold for any future edit to it. And it runs on `ubuntu-latest`, never
`vars.RUNNER_LABEL`, because a job whose purpose is relieving CI pressure must
not queue behind the pool it is relieving.

It gates nothing and reports no required context, the same as
`burst-runner-watchdog.yml`. The no-advisory-tier rule governs checks; this
produces no correctness signal. It still fails loudly rather than warning,
because a silent failure would let the budget refill unnoticed, which is the
condition it exists to prevent.

## Firmware OTA publishing is manual, and the ref you dispatch picks the channel

Firmware OTA reaches devices only when someone manually dispatches
`firmware-build.yml`. Nothing on the push-to-`main` path publishes OTA:
`release-components.yml` runs semantic-release, cuts the `firmware-v*` tag,
builds the per-board factory images and attaches them to the GitHub release,
and stops there. A merged firmware fix is therefore tagged, released, and
downloadable, but it is on no device until that dispatch happens. This is
intentional, not an oversight, so do not read a merged firmware release as
shipped to the mesh.

Two mechanics decide what a dispatch actually does, and both are easy to get
wrong:

- The ref you dispatch on selects the channel. `firmware-build.yml` matches
  the ref name against `^firmware-v(.+)$`: dispatching on a `firmware-v*` tag
  resolves `channel=stable` at that tag's version, and dispatching on anything
  else (`main`, a branch) resolves `channel=dev` at a version derived from
  `scripts/ci-publish-ota.sh --print-version`. There is no channel input, so
  picking the ref is picking the channel.
- A `workflow_dispatch` on a tag runs the workflow file **as of that tag**, not
  the copy on `main`. Workflow fixes merged to `main` do not apply to
  dispatches of tags cut before them; to pick them up, dispatch a tag that
  contains them.

Publishing a stable OTA release is therefore: dispatch Firmware Build on the
`firmware-v*` tag you want on devices, then confirm the new version appears in
the OTA index (the workflow's own verification step fails the run if the
release is incomplete).

## Web client build, publish, and deploy

`.github/workflows/` is the only CI tree in this repo, and it runs on the
self-hosted ARC scale set (runner label from the `RUNNER_LABEL` repo variable).
There is no second copy of these workflows anywhere: the Gitea mirror that once
carried a frozen `.gitea/workflows/` tree is retired, and that tree is gone.

`webapp-build-publish.yml` is the one workflow that is not a check. It stages
the web flasher, builds the unified runtime image, pushes it to the registry
with OCI media types, and reads both published tags back to prove the push
landed. It runs on the `bramble-host` runner, the container-capable persistent
host that carries the docker socket and registry reachability.

It is a reusable workflow, not a push-triggered one. `release-components.yml`
calls it from a `webapp-image` job once semantic-release has cut a `webapp-v*`
tag, so an image is published per released version rather than per merge. A
`workflow_dispatch` with a `webapp_tag` input republishes an existing release
tag. A workflow triggered by `push: tags:` would not work here: semantic-release
pushes the tag with the default `GITHUB_TOKEN`, and a tag pushed by
`GITHUB_TOKEN` does not trigger workflows.

Every step of it gates. The registry push needs the `REGISTRY_HOST` and
`REGISTRY_IMAGE_REPO` repo variables plus the `REGISTRY_USERNAME` and
`REGISTRY_PAT` secrets, and a preflight step fails the run when any of them is
unset rather than falling back to a default target. Nothing is swallowed, so a
green run means the image was pushed and verified in the registry.

Publishing is not deploying. The hosted site rolls forward through GitOps, and
this repo's CI holds no cluster or GitOps credential; see
[deploy-bramble-web-client.md](deploy-bramble-web-client.md).
