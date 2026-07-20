# Contributing to Bramble

Thanks for your interest in Bramble. This document is the practical path from
a fresh clone to a merged pull request: what to install, what to run, and what
CI will check.

- [Where the project lives](#where-the-project-lives)
- [Repo map](#repo-map)
- [First-time setup](#first-time-setup)
- [Install the git hooks (do this first)](#install-the-git-hooks-do-this-first)
- [The quality gates](#the-quality-gates)
- [Branch naming](#branch-naming)
- [Commit messages](#commit-messages)
- [House rules](#house-rules)
- [Opening a pull request](#opening-a-pull-request)
- [Reporting bugs and asking questions](#reporting-bugs-and-asking-questions)

## Where the project lives

GitHub is canonical for outside contributions:
<https://github.com/justinlindh/bramble>. Issues, pull requests, and CI all
live there. Any other remote you may find is a mirror and is not where work
gets reviewed.

```bash
git clone https://github.com/justinlindh/bramble.git
cd bramble
```

Every path in this document is relative to the repository root. Substitute
your own checkout location wherever a command says `cd bramble`.

## Repo map

| Path | What it is |
| --- | --- |
| `components/`, `main/` | ESP-IDF C firmware for ESP32-S3 |
| `webapp/` | TypeScript/React web client plus the Electron desktop shell |
| `simulator/gosim/` | Go mesh simulator that runs the real firmware logic |
| `emulator/` | The firmware built for ESP-IDF's linux target, as virtual nodes |
| `test/` | Host test suites (C, run on your machine, no hardware needed) |
| `api/openapi.yaml` | The RPC contract, CI-enforced against `main/rpc_methods.c` |
| `docs/` | Reference documentation, threat model, build and CI policy |
| `hardware/` | The Bramble Pager v1 KiCad design tree |

You do not need hardware, and you do not need ESP-IDF, to contribute to the
webapp, the simulator, the host tests, or the docs.

## First-time setup

What you need depends on what you want to touch.

**Docs, host tests, and anything C in `components/` or `main/`:** a C toolchain
(gcc or clang), `make`, and `bash`. The host test suite compiles the protocol
code natively and needs no ESP-IDF.

The strict clang-format gate is pinned to an exact version, currently the one
in [`.clang-format-version`](.clang-format-version) (`14.0.6`), because
different clang-format releases disagree on macro and designated-initializer
layout and there is no formatting that satisfies two versions at once (issue
\#161). Get the exact version with `uv`:

```bash
uv tool install "clang-format==$(cat .clang-format-version)"
clang-format --version
```

CI's `Static checks` job asserts this version and fails with a clear message
naming both versions when it does not match. `scripts/lint/run-clang-format-check.sh`
(strict or not) prints the same comparison as a warning rather than a hard
failure, so you still get advisory signal without the exact version installed,
but a warning means its findings may not match what CI reports.

**Webapp:** Node.js 20 or newer, and npm. That is what CI builds and tests
with, it is the `engines` floor in `webapp/package.json`, and the repo root
ships an [`.nvmrc`](.nvmrc) so `nvm use` picks a matching version.

```bash
cd webapp
npm ci
```

More detail, including the dev server and the WiFi-transport backend, is in
[webapp/README.md](webapp/README.md).

**Simulator:** Go and a C toolchain (gosim compiles real firmware C into the Go
binary via cgo). `simulator/gosim/go.mod` sets the language floor; CI builds
with a newer patch release, and any Go at or above the `go` directive works.

```bash
cd simulator/gosim
go build ./...
```

**Firmware builds and flashing:** ESP-IDF v5.4.1, the exact tag CI builds
with. The pin lives in `.esp-idf-version` at the repo root and
`scripts/lint/check-idf-version.sh` gates every reference against it. See
[docs/BUILDING.md](docs/BUILDING.md). Flash real hardware only through
`scripts/flash.sh` or `scripts/flash-all.py`, never raw `esptool`.

**Emulator:** ESP-IDF with the **linux** target installed (`install.sh linux`,
not `install.sh esp32s3`), plus Go, Node, and `jq`. Run
`bash emulator/scripts/check_prereqs.sh` and it will tell you exactly what is
missing. A Docker path with zero host prerequisites also exists; see
[emulator/README.md](emulator/README.md).

If something in this section stalls, check
[docs/troubleshooting.md](docs/troubleshooting.md) before digging in. The
common ones (serial permissions on Linux, the wrong ESP-IDF target, port
collisions, quality targets failing silently) are all documented there.

## Install the git hooks (do this first)

The repo ships a pre-commit hook that catches, locally, several things CI
rejects. It is **not** installed by cloning. Run this once per clone:

```bash
make setup-hooks
```

That sets `core.hooksPath` to the tracked `githooks/` directory. Verify it:

```bash
$ git config core.hooksPath
githooks
```

The hook then checks, on every commit:

- clang-format on staged C and H files (skipped if `clang-format` is absent),
- em dashes in added lines (banned repo-wide, see below),
- `make check-fast`, which is the webapp typecheck plus unit tests.

Because the hook runs `make check-fast`, it needs `webapp/node_modules` to
exist. Run `npm ci` in `webapp/` once, even if you are not touching the
webapp, or your commits will fail on a missing dependency rather than on your
change.

Skipping the hooks means your first PR fails CI on rules your machine never
checked. Install them.

## The quality gates

These are the same commands CI runs. Run the ones that cover what you changed;
run all of them if you are unsure.

| What | Command |
| --- | --- |
| Host test suites (always) | `bash test/run_all_tests.sh` |
| Webapp | `cd webapp && npm ci && npx tsc --noEmit && npx vitest run` |
| Simulator | `cd simulator/gosim && go build ./... && go test ./...` |
| Emulator scenarios | `bash emulator/ci/run_scenarios.sh` |
| RPC contract | `bash scripts/check-rpc-contract.sh` |
| Markdown lint | `bash scripts/lint/run-markdownlint.sh` |
| No internal references | `bash scripts/lint/check-no-internal-refs.sh` |

Two notes on running them:

- The emulator scenarios are deterministic by construction. If one flakes, fix
  the flake; do not add a retry loop to absorb it.
- The `make ci-quality-*` targets are convenience wrappers around parts of the
  above. They currently exit non-zero without a message when a required tool
  (`shellcheck`, `cppcheck`, `actionlint`, `uvx`) is not installed. That is a
  missing tool, not a failing check; see
  [docs/troubleshooting.md](docs/troubleshooting.md).

Every CI check gates. There is no advisory tier. A step may use
`continue-on-error` only so that later steps can collect more failures, and
the job must then end with a terminal step that fails if any step failed. CI
topology and the full required-context list are in [docs/ci.md](docs/ci.md)
and [docs/quality-policy.md](docs/quality-policy.md).

## Branch naming

Never push to `main`; it is protected. Branch, then open a pull request.

Use one of these prefixes:

- `fix/**` for bug fixes
- `feat/**` for new functionality
- `chore/**` for maintenance and housekeeping
- `ci/**` for workflow and CI changes
- `docs/**` for documentation

```bash
git switch -c fix/short-description
```

## Commit messages

Conventional Commits, enforced by commitlint on every commit in a PR and on
the PR title:

```text
type(scope): subject
```

Types: `feat`, `fix`, `refactor`, `chore`, `docs`, `test`, `perf`, `ci`.

The scope is optional, but when present it must be in the `scope-enum` list in
[`commitlint.config.cjs`](commitlint.config.cjs), which is authoritative.
Commonly used scopes: `firmware`, `webapp`, `ui`, `settings`, `protocol`,
`sim`, `emulator`, `rpc`, `hardware`, `tooling`, `docs`, `ci`, `security`,
`release`, `crypto`, `radio`, `routing`, `test`. If none fit, add the new
scope to `scope-enum` in the same PR rather than inventing one inline, which
commitlint rejects.

Scope choice has a release consequence. Releases are cut per component by
semantic-release from these messages, so `feat(firmware)` cuts a firmware
minor and `fix(webapp)` a webapp patch. Pick the scope of the component you
actually changed. A squash-expander counts the bullets in squash-merge
bodies, so per-commit messages matter even in a squashed PR.

## House rules

These are enforced, so they are worth reading before you write anything.

**No em dashes.** Anywhere: code, comments, docs, commit messages, PR bodies.
Use a colon, a comma, or restructure the sentence. A pre-commit hook and a CI
check both reject the character.

**No AI attribution.** No "generated with" footers, no `Co-Authored-By`
trailers for assistants, no chat session links, in commits or PR bodies. If
your tooling appends one automatically, strip it before you push.

**This repository is public.** Run
`bash scripts/lint/check-no-internal-refs.sh` before every push and fix
anything it flags. Never commit internal hostnames, real device addresses,
private LAN addresses, personal filesystem paths, or precise real-world
coordinates. Use documentation placeholders: RFC 5737 addresses such as
`192.0.2.100`, `AA:BB:CC:DD:EE:FF` for MACs, and fictional coordinates in
fixtures.

**Honesty conventions.** State what is verified and how: which suite, which
board. Simulation results are labeled as simulation. Plans that have not been
executed are not documented as capabilities. If a claim cannot be verified, it
does not ship. This applies to documentation and to PR descriptions equally.

**Respect other projects.** Comparisons with Meshtastic and MeshCore are
factual, sourced, and respectful. They are not competitors, and copy that
frames them as inferior does not ship. See [docs/COMPARISON.md](docs/COMPARISON.md)
for the house style.

**One theme per PR.** A docs change touches docs. Unrelated changes riding
along make review harder and have reverted other people's fixes in the past.

## Opening a pull request

Open it against `main`. The PR body follows
[`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md), which
has four sections:

1. **What and why:** the problem or motivation, then the approach. For a bug
   fix, describe the failure mode concretely.
2. **Changes:** brief, at the file or area level.
3. **Validation:** evidence, not assertions. Name the command and paste the
   real result. If CI is your only validation because you lack a local
   toolchain or the hardware, say so explicitly rather than implying you ran
   it.
4. **Release impact:** breaking, user-visible, or none.

One formatting quirk to know: write PR and issue bodies as **unwrapped**
prose, one paragraph per line. GitHub's renderer treats every newline as a
line break, so hard-wrapped paragraphs render as ragged single-spaced lines.
This applies only to PR and issue bodies. Markdown files inside the repo wrap
normally at about 80 columns.

The PR title becomes the squash-commit subject, so it must itself be a valid
conventional commit with a valid scope.

Required checks gate the merge. If a check fails, read its log rather than
re-running it; the gates are deterministic.

## Reporting bugs and asking questions

- **Bugs and feature requests:** open an
  [issue](https://github.com/justinlindh/bramble/issues/new/choose). The
  templates ask for the board, firmware version, and reproduction steps
  because for a radio mesh those are usually the whole diagnosis.
- **Questions, ideas, and show-and-tell:**
  [Discussions](https://github.com/justinlindh/bramble/discussions).
- **Security vulnerabilities:** do not open a public issue. Follow
  [SECURITY.md](SECURITY.md).

Everyone taking part is expected to follow the
[Code of Conduct](CODE_OF_CONDUCT.md).
