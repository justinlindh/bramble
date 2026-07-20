#!/usr/bin/env bash
# Required gate: markdownlint-cli2 over every tracked markdown file, using the
# root .markdownlint-cli2.yaml config (globs and ignores live there, not here).
# There is no advisory mode: this script's predecessor defaulted to reporting
# findings and exiting 0, which docs/quality-policy.md forbids, and it was
# deleted rather than wired into CI for exactly that reason (issue #160). A
# real gate needed the tree reformatted first; that sweep landed alongside
# this script.
#
# markdownlint-cli2 is expected to be pre-baked on the runner image, the same
# contract as shellcheck/clang-format/cppcheck/actionlint/uv in the Static
# checks job. A missing tool is a runner-image defect, not something this
# script papers over by fetching a copy at CI time.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

if ! command -v markdownlint-cli2 >/dev/null 2>&1; then
  echo "run-markdownlint: markdownlint-cli2 not found on PATH." >&2
  echo "Install a pinned copy, e.g.: npm install -g markdownlint-cli2@0.23.1" >&2
  echo "(or run via npx: npx --yes markdownlint-cli2@0.23.1 ...)" >&2
  exit 1
fi

# No glob arguments: .markdownlint-cli2.yaml's globs/ignores are authoritative
# (repo-wide **/*.md, excluding build output, vendored trees, and docs/archive).
markdownlint-cli2
