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
  - ESP-IDF version pins (`bash scripts/lint/check-idf-version.sh`: every ESP-IDF reference in the tree must match `.esp-idf-version`)
  - Node version pins (`bash scripts/lint/check-node-version.sh`: every `actions/setup-node` pin, Node base image, `engines` floor and setup doc must match the major in `.nvmrc`, so the published container image and the desktop installers cannot drift onto different Node majors)
  - ARM toolchain version pins (`bash scripts/lint/check-arm-gcc-version.sh`: every reference to the nRF52840 cross-compiler must match `.arm-gcc-version`, and the CI assert and the nRF build must read that file rather than restate the version). This pin is load-bearing rather than cosmetic: the `nRF52840 build` job's memory gate fails on a byte count, GCC releases produce different byte counts from identical source, and the difference exceeds the headroom the T1000-E build runs at, so an unpinned local toolchain yields a size number that looks like CI's and is not. Matching the version is necessary but not sufficient for byte-exact agreement, since the bundled newlib differs between distributions of the same GCC; `nrf/README.md` documents the container that reproduces CI's counts, and CI remains the authority on them
  - Strict shellcheck (`bash scripts/lint/run-shellcheck.sh --strict`)
  - Ruff baseline profile (`ruff check scripts --select E9,F63,F7,F82`)
  - Strict clang-format, full scope (`bash scripts/lint/run-clang-format-check.sh --strict`), pinned to the version in `.clang-format-version` (currently 14.0.6); the `Static checks` job asserts the runner's `clang-format --version` matches before running the check, and the wrapper script prints a warning if your local version differs, because unpinned clang-format versions disagree on macro and designated-initializer layout and produce unreproducible findings (issue #161)
  - cppcheck (`--error-exitcode=2`, blocking)
  - RPC contract check (`bash scripts/check-rpc-contract.sh`: `api/openapi.yaml` vs the firmware registry in `main/rpc_methods.c`)
  - Markdownlint (`bash scripts/lint/run-markdownlint.sh`, issue #160): `markdownlint-cli2` against the root `.markdownlint-cli2.yaml` config, over every tracked markdown file. The runner image bakes the tool (issue #222, image 1.2.0); the script asserts the PATH binary matches its pinned version and fails loud on drift, falling back to a pinned `npx --yes markdownlint-cli2@0.23.1` only where no binary is on PATH
  - Actionlint over every workflow file
- `Host tests` (`bash test/run_all_tests.sh`, `quality.yml`)
- `Parser fuzzing` (`bash test/fuzz/run_fuzz.sh`, `quality.yml`): a 30-second-per-target libFuzzer campaign, under ASan and UBSan, over the wire-frame parsers in `components/packet/packet.c` and the fragment reassembler. Both harnesses start from committed seed corpora (`test/fuzz/corpus/`) built from the host suites' own test vectors. Requires `clang` with libFuzzer on the runner image; the job asserts the toolchain is present and fails hard when it is not, because a skip here would be an advisory check in disguise.
- `Release config` (`quality.yml`): two step-gated release/publish node tests sharing one node pod. The release-rule scope-gating regression (`node scripts/release/semantic-release-squash-expander.test.cjs`, gated on `release_config`) runs against the real `.releaserc.<component>.cjs` files, so a config change that would silently stop every component release fails the PR. The OTA firmware-index validator (`node test/test-validate-firmware-index.js`, gated on `ota_index`) pins `scripts/validate-firmware-index.js` to `docs/ota-release-schema.md`, so a schema regression that would ship an `index.json` the OTA journey cannot consume fails the PR
- `nRF52840 build` (`quality.yml`): the bare-metal nRF52840 target (`nrf/`, bare FreeRTOS + nrfx, no ESP-IDF) built with `arm-none-eabi-gcc 13.2.1`, baked into the runner image and asserted against `.arm-gcc-version` before anything compiles. The post-link step runs `nrf/scripts/size_report.py`, which fails the build on any of three limits on the 256KB chip: total RAM demand over 252KB, static (non-heap) RAM over 104KB, or the FreeRTOS heap below its 144KB floor. The floor is what stops new statics being paid for by shrinking the heap, which would move the shortfall to runtime. The memory gate is inseparable from the build, and from the compiler: the report names the toolchain behind its numbers because a report measured with a different one is a different measurement, not a second opinion. Gates on `firmware`, `nrf`, or `ci_core`: the target compiles `components/` sources directly
- `gosim integration` (builds and tests the simulator, `quality.yml`)
- `Board build smoke (heltec-v3)`, `(tdeck-plus)`, `(heltec-v4)`, `(bramble-pager)` (`quality.yml`): one context per board, each running `bash scripts/flash.sh local <board> build`. `fail-fast: false`, so every board reports its own result. The area gate (`firmware`, `idf_build_scripts`, `size_tooling`, or `ci_core`) is on the job's steps rather than on the job, because a false job-level `if:` prevents GitHub from expanding the matrix at all and collapses the four contexts into one check run named with the raw literal `Board build smoke (${{ matrix.board }})`. Step-level gating keeps all four contexts reporting on every PR, so they are safe to require
- `Emulator suite` (`quality.yml`): the merged emulator scenario suite plus
  browser E2E. Uses the collect-then-fail pattern (below), so both suites
  always report and both gate.
- `Docker build (webapp)`, `(simulator)`, `(emulator)`, `(firmware-builder)` (`quality.yml`, issue #195): one context per shipped Dockerfile, each a `docker build` on the runner host, never pushed. Same step-level area-gate pattern as `Board build smoke`, with `ci_core` forcing every leg: `webapp` gates on `webapp` or `web_flasher` (the shipped webapp image bundles `web-flasher/`, so a web-flasher-only change must rebuild-verify this leg); `simulator` gates on `simulator` or `firmware`; `emulator` gates on `emulator`, `simulator`, or `firmware`; `firmware-builder` gates on `docker_firmware_builder` alone (its build context is a toolchain image that does not copy firmware source, so a firmware change must not rebuild it). Makes Dependabot's docker-ecosystem bumps self-verifying instead of merging on unrelated green checks with nothing having built the image. The emulator leg builds with `--network=host` (a `netmode: host` matrix field) so its ESP-IDF stage can reach `components-file.espressif.com`, which is reachable from the runner host but not the build's default bridge network; this is the same live-fetch model the required `Emulator suite` and the other docker legs already use, on a trusted CI runner building first-party images.
- `Webapp checks` (one pod: typecheck, electron typecheck, unit tests, build, e2e smoke, `webapp-quality.yml`). No lint step: the webapp has no linter configured, and the former Lint step was a duplicate of the typecheck under a name that claimed otherwise.
- `web-flasher tests` (`node --test 'web-flasher/**/*.test.js'`, `webapp-quality.yml`)

There is no clang-tidy gate. The `run-clang-tidy-advisory.sh` wrapper used to
sit in `scripts/lint/` unreferenced by any workflow, Makefile target, or
script, and defaulted to reporting findings and then exiting 0, which is
exactly the advisory tier this policy forbids. It was deleted rather than
promoted: a real clang-tidy gate needs an xtensa-capable toolchain and a
firmware build in the static pod. The `.clang-tidy` dotfile stays because
editors and language servers read it directly; run `clang-tidy` by hand if
you want the local signal.

Markdownlint used to be in the same boat (`run-markdownlint.sh` deleted for
the same advisory-exit-0 reason, tracked as issue #160), but it is now a real
gate: the tree was reformatted to satisfy `.markdownlint-cli2.yaml` and
`scripts/lint/run-markdownlint.sh` runs as a required step in `Static checks`
(see above). `.markdownlint-cli2.yaml`'s `MD024` (`siblings_only: true`),
`MD036`, and `MD060` overrides exist because those three rules would otherwise
flag legitimate, repeated repo patterns (per-section subject headings,
bold field labels used as flat in-section sub-labels in spec docs, and
hand-maintained table pipe spacing) rather than real defects; see the config
file's own comments for the specific rationale per rule.

## Coverage ratchet (line coverage, no-regression)

Three suites measure line coverage and gate on it as steps inside existing
required jobs (no new required context):

- `host-c`: a second gcov-instrumented build of the Unity host suite
  (`BRAMBLE_COVERAGE=ON`, ASan off) built into `test/build-coverage/`, run inside
  the `Host tests` job (`quality.yml`). `scripts/ci/host_coverage.py` aggregates
  line coverage over product code (`components/`, `main/`) using only `gcov` and
  the Python standard library, so no `gcovr`/pip install can strand the gate.
- `gosim`: `go test -covermode=set` total statement coverage, in the
  `gosim integration` job (`quality.yml`).
- `webapp`: vitest v8 line coverage collected in the same `Unit tests` run
  (`--coverage`), in the `Webapp checks` job (`webapp-quality.yml`).

Each suite's measured percentage is checked by `scripts/ci/check_coverage.py`
against a committed floor in `ci/coverage-baseline.json`. The gate fails when
coverage drops more than `tolerance_pct` below the floor, which is what deleting
a test does. The baseline NEVER auto-drifts: it is a checked-in file a developer
updates deliberately with `scripts/ci/update-coverage-baseline.sh` (floors go up
after adding tests; a floor only goes down with a written justification in the
PR). The host-c number is gcc-version sensitive, so the canonical baseline is the
value the runner measures; when a first CI run reports a different number than a
local box, commit the CI value.

## Firmware size ratchet (flash and static RAM, no-regression)

Each board leg of the `board-build-smoke` matrix (`quality.yml`) ratchets the
firmware it just built against a committed ceiling in `ci/size-baseline.json`.
`scripts/ci/check-firmware-size.sh <board>` measures two numbers from
`idf.py size --format json`: the app flash footprint (`flash_code` plus
`flash_rodata` plus `flash_other`) and static DRAM (initialized `.data` plus
`.bss`, the RAM the app reserves before the heap). It uses the UNPADDED flash
figure, not the `bramble.bin` image size, because the image is padded up to
64 KiB ESP32-S3 MMU pages, so its size jumps a whole page when a segment crosses
a boundary and can differ by 64 KiB between ESP-IDF patch versions; the unpadded
figure moves precisely with the code and data a change adds, and partition fit is
already enforced by the ESP-IDF build.
It fails when either metric grows more than `tolerance_bytes` above the ceiling,
naming exactly what grew and by how much. This runs at PR time because the board
build itself now gates PRs: this project shipped a main-task stack overflow that
only hardware caught, and RAM headroom is the documented T1000-E port blocker, so
a silent flash bloat or a shrinking RAM margin must surface before merge, not when
a device bootloops.

Like the coverage baseline, `ci/size-baseline.json` NEVER auto-drifts: a
developer raises a ceiling deliberately with `scripts/ci/update-size-baseline.sh`
when a change legitimately grows the binary, with the reason in the PR. Sizes are
toolchain-sensitive, so `tolerance_bytes` (8 KiB) absorbs the sub-KiB unpadded
delta between ESP-IDF patch versions while still catching the tens-of-KiB
regressions this gate exists to stop.

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
actionlint -color -oneline -ignore 'shellcheck reported issue.*SC2317' -config-file .actionlint.yaml .github/workflows/*.yml

bash scripts/lint/check-no-internal-refs.sh
bash scripts/lint/check-no-em-dash.sh
bash scripts/lint/run-markdownlint.sh         # uses markdownlint-cli2 on PATH if present,
                                               # else falls back to a pinned npx invocation

# Local-only helpers (not wired into CI)
bash scripts/lint/run-clang-format-check.sh
bash scripts/lint/run-shellcheck.sh
bash scripts/flash.sh local heltec-v3 build
```
