#!/usr/bin/env bash
# Required gate: markdownlint-cli2 over every tracked markdown file, using the
# root .markdownlint-cli2.yaml config (globs and ignores live there, not here).
# There is no advisory mode: this script's predecessor defaulted to reporting
# findings and exiting 0, which docs/quality-policy.md forbids, and it was
# deleted rather than wired into CI for exactly that reason (issue #160). A
# real gate needed the tree reformatted first; that sweep landed alongside
# this script.
#
# Pinned rather than baked into the runner image (that's issue #222's separate
# scope): if a pre-installed markdownlint-cli2 is on PATH it is used as-is
# (e.g. once #222 bakes it in), otherwise this falls back to a pinned version
# via npx, the same "pin at the call site" idiom the ruff step in
# firmware-quality.yml uses via uvx. `npm install -g` was tried first and
# rejected: the runner image's global npm prefix (/usr/local/lib/node_modules)
# is not writable by the job user, and its baked Node (v20) predates the
# package's declared >=22 engines requirement anyway (an EBADENGINE warning,
# not a hard failure; verified the package still runs correctly under Node 20).
# npx sidesteps both problems by caching under the user's own npm cache.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

MARKDOWNLINT_CLI2_VERSION="0.23.1"

if command -v markdownlint-cli2 >/dev/null 2>&1; then
  lint_cmd=(markdownlint-cli2)
elif command -v npx >/dev/null 2>&1; then
  lint_cmd=(npx --yes "markdownlint-cli2@${MARKDOWNLINT_CLI2_VERSION}")
else
  echo "run-markdownlint: neither markdownlint-cli2 nor npx found on PATH." >&2
  echo "Install Node, then run: npx --yes markdownlint-cli2@${MARKDOWNLINT_CLI2_VERSION}" >&2
  exit 1
fi

# No glob arguments: .markdownlint-cli2.yaml's globs/ignores are authoritative
# (repo-wide **/*.md, excluding build output, vendored trees, and docs/archive).
"${lint_cmd[@]}"
