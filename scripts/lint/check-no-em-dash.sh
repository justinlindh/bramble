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

# U+2014 is built from its UTF-8 bytes at runtime, for two reasons: this file
# must not contain the character it forbids (it would flag itself), and a
# fixed-string search works on every git grep, while PCRE escapes like
# \x{2014} are rejected by some PCRE2 builds ("character code point value in
# \x{} or \o{} is too large"), which would otherwise turn this gate into a
# silent pass on those machines.
em_dash="$(printf '\342\200\224')"

set +e
hits="$(git grep -nIF "$em_dash" -- . \
  ':!node_modules' \
  ':!**/node_modules' \
  ':!**/managed_components' \
  ':!simulator/engine/cJSON.c')"
rc=$?
set -e

# git grep: 0 = matches found, 1 = no matches, anything else is an error and
# must fail the gate rather than read as clean.
if [[ $rc -ge 2 ]]; then
  echo "check-no-em-dash: git grep failed with exit code $rc" >&2
  exit "$rc"
fi

if [[ $rc -eq 0 ]]; then
  printf 'check-no-em-dash: em dash (U+2014) found; use a colon, a comma, or restructure:\n%s\n' "$hits" >&2
  exit 1
fi

echo "check-no-em-dash: clean"
