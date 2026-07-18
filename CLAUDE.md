# CLAUDE.md

Guidance for AI assistants (and humans in a hurry) working in this repo.

## What this is

Bramble: a from-scratch LoRa mesh protocol and firmware for ESP32-S3, with a
web/desktop client, Go simulator, and emulator. Pre-alpha, hardware-verified
on a small bench mesh; scale numbers come from simulation and are published
honestly. Repo map: `components/` + `main/` (ESP-IDF C firmware), `webapp/`
(TypeScript/React client + Electron), `simulator/gosim/` (Go mesh simulator
running the real firmware logic), `emulator/` (firmware built for the linux
target), `test/` (host test suites), `scripts/`, `docs/`, `api/openapi.yaml`
(the RPC contract).

## Hard rules

- No em dashes anywhere: code, docs, commits, PR bodies. Use a colon, comma,
  or restructure.
- Conventional commits: `type(scope): subject`. Scopes in use: firmware,
  webapp, ui, settings, protocol, sim, emulator, rpc, hardware, tooling,
  docs, ci, security, release, deps, deps-dev. The PR title becomes the
  squash-commit subject; commitlint gates it and semantic-release derives
  component versions from it.
- No AI attribution: no generated-with footers, no co-author trailers, no
  session links, in commits or PR bodies. Tooling may auto-append a footer
  to PR bodies; verify after `gh pr create` and strip it with `gh pr edit`.
- Never push to `main`. Branch (`fix/**`, `feat/**`, `chore/**`, `ci/**`,
  `docs/**`) and open a PR; required checks gate the merge.
- This repo is public. Before every push run
  `bash scripts/lint/check-no-internal-refs.sh` and fix anything it flags.
  Never commit internal hostnames, real device addresses, private LAN
  addresses, personal filesystem paths, or precise real-world coordinates.
  Use documentation placeholders and fictional coordinates in fixtures.
- Respect other projects. Comparisons with Meshtastic and MeshCore are
  factual, sourced, and respectful; they are not competitors, and copy that
  frames them as inferior does not ship. See `docs/COMPARISON.md` for the
  house style.
- Every CI check gates. There is no advisory tier. A step may use
  continue-on-error only to let later steps collect more failures, and the
  job must then end with a terminal step that fails on any failed step.
- PR bodies follow `.github/PULL_REQUEST_TEMPLATE.md` (What and why /
  Changes / Validation with real command output / Release impact), written
  as unwrapped prose: one paragraph per line, because the GitHub renderer
  treats every newline as a line break.

## Build and test

- Host test suites (required gate): `bash test/run_all_tests.sh`
- Webapp: `cd webapp && npm ci && npx tsc --noEmit && npx vitest run`
- Simulator: `cd simulator/gosim && go build ./... && go test ./...`
- Emulator scenario suite: `bash emulator/ci/run_scenarios.sh` (builds the
  firmware's linux target; scenarios are deterministic by construction, do
  not add retry loops to absorb flake, fix the flake)
- RPC contract: `bash scripts/check-rpc-contract.sh` (`api/openapi.yaml`
  must list exactly the methods `main/rpc_methods.c` registers)
- Firmware builds need ESP-IDF v5.4.1; CI sources it via
  `scripts/ci-source-idf.sh`. Flash real hardware only through
  `scripts/flash.sh` or `scripts/flash-all.py`, never raw esptool.

## CI and releases

CI topology, the change-detection contract (workflows always trigger; heavy
jobs skip per area but their contexts always report), and the required
context list live in `docs/ci.md` and `docs/quality-policy.md`. Releases are
automated per component by semantic-release from conventional commits
(`.releaserc.*.cjs`); a squash-expander counts the bullets in squash-merge
bodies, so per-commit messages matter even in squashed PRs. Dependency PR
scopes `deps`/`deps-dev` never cut releases.

## Honesty conventions

State what is verified and how (which suite, which hardware). Simulation
results are labeled as simulation. Unexecuted plans are not documented as
capabilities. When a claim cannot be verified, it does not ship.
