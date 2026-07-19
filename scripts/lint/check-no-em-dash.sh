#!/usr/bin/env bash
# Anti-regression gate: fails if the tracked tree carries the em dash
# character (U+2014). CLAUDE.md bans it repo-wide, in code, docs, commits,
# and PR bodies; use a colon, a comma, or restructure the sentence instead.
# githooks/pre-commit checks the same character but only on added lines in
# staged changes, and only for contributors who opted in via
# `make setup-hooks`, so this script is the gate that actually holds.
#
# Excluded: vendored trees we do not author. node_modules and
# managed_components are third-party dependency checkouts and
# simulator/engine/cJSON.c is a vendored upstream source. There are no
# first-party exemptions on purpose: an exemption list is how a gate decays
# into an advisory tier, which docs/quality-policy.md forbids.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# U+2014 is spelled as an escape on purpose: this file must not contain the
# character it forbids, or it would flag itself.
hits="$(git grep -nIP '\x{2014}' -- . \
  ':!node_modules' \
  ':!**/node_modules' \
  ':!**/managed_components' \
  ':!simulator/engine/cJSON.c' || true)"

if [[ -n "$hits" ]]; then
  printf 'check-no-em-dash: em dash (U+2014) found; use a colon, a comma, or restructure:\n%s\n' "$hits" >&2
  exit 1
fi

echo "check-no-em-dash: clean"
