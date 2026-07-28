# CLAUDE.md

Guidance for AI assistants (and humans in a hurry) working in this repo.

## What this is

Bramble: a from-scratch LoRa mesh protocol and firmware for ESP32-S3, with a
web/desktop client, Go simulator, and emulator. Early but functional: the
stack is implemented end to end, reviewed, host-tested, and running stable on
a small bench mesh; scale numbers come from simulation and are published
honestly. Repo map: `components/` + `main/` (ESP-IDF C firmware), `webapp/`
(TypeScript/React client + Electron), `simulator/gosim/` (Go mesh simulator
running the real firmware logic), `emulator/` (firmware built for the linux
target), `nrf/` (bare-metal nRF52840 target, P2: LR1110 mesh + BLE RPC +
flash persistence), `test/` (host test
suites), `scripts/`, `docs/`, `api/openapi.yaml` (the RPC contract).

## Hard rules

- No em dashes anywhere: code, docs, commits, PR bodies. Use a colon, comma,
  or restructure.
- Conventional commits: `type(scope): subject`. The scope must be one of
  the `scope-enum` values in `commitlint.config.cjs`: that list is
  authoritative and CI-gated, so treat it as the source of truth rather
  than this bullet. Pick the closest existing scope (common ones:
  firmware, webapp, ui, settings, protocol, sim, emulator, rpc, hardware,
  tooling, docs, ci, security, release, deps, deps-dev); if none fit, add
  the new scope to `scope-enum` in the same PR rather than inventing one
  inline, which commitlint will reject. The PR title becomes the
  squash-commit subject; commitlint gates it and semantic-release derives
  component versions from it.
- No AI attribution: no generated-with footers, no co-author trailers, no
  session links, in commits or PR bodies. Tooling may auto-append a footer
  to PR bodies; verify after `gh pr create` and strip it with `gh pr edit`.
- Never push to `main`. Branch (`fix/**`, `feat/**`, `chore/**`, `ci/**`,
  `docs/**`) and open a PR; required checks gate the merge.
- Trunk-based, and nothing incomplete lands on trunk. Incomplete means the
  change leaves known work behind: a follow-up, a tracked-separately, a
  deferred leg, a naive fix with a noted caveat, a TODO for the real fix.
  When you find a defect or a needed change while working, fix it fully and
  properly now, in the same change, not later. Filing a GitHub issue to
  defer work you could do now is not allowed, and issues are strongly
  discouraged in general; do not create them to park scope. If a fix
  broadens the PR's scope, broaden the PR: widen the branch, or stack under
  an integration branch that only merges to trunk once complete. A latent
  bug a change surfaces (for example a Dockerfile that was never buildable
  from a clean checkout) gets fixed in that change, not deferred. A reviewer
  finding that is real is addressed before merge, never merged with a
  follow-up pointer. This is non-negotiable: deferral in any form is not
  allowed, ever. The one narrow exception is genuinely unrunnable
  verification, for example firmware you cannot flash without a hardware
  bench, which is an honest constraint, not deferred code work.
- Never force-push. Not `git push --force`, not `--force-with-lease`, not by
  any other route. History on a pushed branch is append-only. To bring a
  branch up to date with main, merge `github/main` into it (a merge commit
  is fine, the squash-merge flattens it at the end); never rebase-and-force
  a pushed branch. Rebasing is only acceptable on a branch you have not
  pushed yet. A force-push has already silently dropped a merged PR's
  content on this repo, which is exactly what this rule prevents.
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
- CI does no redundant or unnecessary work. Never run the same expensive
  step twice: fold coverage or other instrumentation into the existing test
  run rather than re-running a suite to produce a second artifact or number.
  Never run a job or suite a change cannot affect: respect and tighten the
  change-detection areas so, for example, a docs-only or webapp-only PR does
  not build firmware. Prefer the cheapest correct mechanism. Running a
  timing-sensitive suite twice also doubles the flake surface, which is its
  own reason to avoid it. Any CI change is reviewed specifically for
  duplicated or unnecessary work.
- PR bodies follow `.github/PULL_REQUEST_TEMPLATE.md` (What and why /
  Changes / Validation with real command output / Release impact), written
  as unwrapped prose: one paragraph per line, because the GitHub renderer
  treats every newline as a line break. The complement matters just as much:
  put a BLANK line between every paragraph, heading, and list, because
  adjacent single-line paragraphs fuse into one unreadable wall (a single
  newline renders as a soft break, not a paragraph break). Prefer a bullet
  list over a multi-clause paragraph for the Changes section; a paragraph
  that needs three semicolons is a list.
- PR bodies are written for a human reviewer, not as a log. Keep them
  concise and describe the CURRENT state of the change, not the history of
  how it got there. No running monologue, no per-iteration narration, no
  rebase or CI-flake or incident commentary, no restating the diff line by
  line. Keep each template section to the minimum a reviewer needs: What and
  why in a sentence or two, Changes as the load-bearing points, Validation
  as the commands run and their results. Before merge, edit the body to its
  final state; if the PR evolved, rewrite the body, do not append to it.

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
  `scripts/ci-source-idf.sh`. The version pin is a single source of truth in
  `.esp-idf-version` and `scripts/lint/check-idf-version.sh` fails CI if any
  doc, Dockerfile, or script disagrees with it, so bumping ESP-IDF means
  editing that file plus every reference the script names. Flash real hardware only through
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
