<!--
Title format (required): type(scope): subject
  Types: feat, fix, refactor, chore, docs, test, perf, ci
  Scopes: firmware, webapp, ui, settings, protocol, sim, emulator, rpc,
          hardware, tooling, docs, ci, security, release
The title becomes the squash-commit subject: commitlint validates it and
semantic-release derives component versions from it (feat(firmware) cuts a
firmware minor, fix(webapp) a webapp patch, and so on). Choose the scope of
the component you actually changed.
-->

## What and why

<!-- The problem or motivation, then the approach. If this fixes a bug,
describe the failure mode concretely: inputs, wrong behavior, impact. -->

## Changes

<!-- Brief, file-or-area level. One coherent theme per PR; split unrelated
changes into separate PRs. -->

## Validation

<!-- Evidence, not assertions. Name what you ran and the result, e.g.:
- bash test/run_all_tests.sh: 119/119
- cd webapp && npx vitest run: pass
- cd simulator/gosim && go test ./...: pass
- bash scripts/lint/check-no-internal-refs.sh: clean
Hardware-verified changes: name the board and what you observed. If CI is
the only validation (e.g. no local toolchain), say so explicitly. -->

## Release impact

<!-- Delete the lines that do not apply.
- Breaking change (major): what breaks and the migration path
- User-visible behavior change: what users will notice
- None: internal only -->

<!-- House rules (enforced by CI, listed to save you a round trip):
conventional title with a valid scope; no em dashes anywhere; no AI
attribution (no Generated-with footers, no Co-Authored-By trailers, no
session links); public repo, so no internal hostnames, real device
addresses, private IPs, or precise real-world coordinates. -->
