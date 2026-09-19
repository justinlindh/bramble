#!/usr/bin/env bash
# Required gate: markdownlint-cli2 over every tracked markdown file, using the
# root .markdownlint-cli2.yaml config (globs and ignores live there, not here).
# There is no advisory mode (docs/quality-policy.md, issue #160).
#
# The linter version is pinned below and must match the markdownlint-cli2 baked
# into the CI runner image (bramble#222). A PATH binary at the pinned version is
# used directly. In CI anything else is runner-image drift and fails, so this
# required gate never depends on the npm registry and never produces findings
# no local run can reproduce. Outside CI the pinned version is fetched through
# npx instead, so a machine with a different global install still lints with
# the pinned one. The package declares engines node >=22, which the repo's
# pinned Node major satisfies.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

MARKDOWNLINT_CLI2_VERSION="0.23.1"

found_version="none"
if command -v markdownlint-cli2 >/dev/null 2>&1; then
  # Resolve the installed package version through the bin symlink: the npm
  # global bin links to a script inside .../node_modules/markdownlint-cli2/,
  # so walk up from the resolved script to the package directory.
  bin_real="$(realpath "$(command -v markdownlint-cli2)")"
  pkg_dir="$(dirname "$bin_real")"
  while [ "$pkg_dir" != "/" ] && [ "$(basename "$pkg_dir")" != "markdownlint-cli2" ]; do
    pkg_dir="$(dirname "$pkg_dir")"
  done
  found_version="$(node -p "require('${pkg_dir}/package.json').version" 2>/dev/null || echo unknown)"
fi

if [ "${found_version}" = "${MARKDOWNLINT_CLI2_VERSION}" ]; then
  lint_cmd=(markdownlint-cli2)
elif [ "${GITHUB_ACTIONS:-}" = "true" ]; then
  echo "::error::markdownlint-cli2 on PATH is ${found_version}, pinned version is ${MARKDOWNLINT_CLI2_VERSION}. Align the runner-image bake with scripts/lint/run-markdownlint.sh."
  exit 1
elif command -v npx >/dev/null 2>&1; then
  lint_cmd=(npx --yes "markdownlint-cli2@${MARKDOWNLINT_CLI2_VERSION}")
else
  echo "run-markdownlint: markdownlint-cli2 on PATH is ${found_version}, pinned version is ${MARKDOWNLINT_CLI2_VERSION}, and there is no npx to fetch it." >&2
  echo "Install Node, then run: npx --yes markdownlint-cli2@${MARKDOWNLINT_CLI2_VERSION}" >&2
  exit 1
fi

# Lint exactly the TRACKED markdown files. The config's repo-wide `**/*.md`
# glob would also lint untracked markdown on disk (gitignored build output such
# as webapp/dist/, agent worktrees, scratch files), so a tree that is clean in
# CI could fail locally. Passing the tracked list as CLI globs disables the
# config's `globs:` but keeps its `ignores:` (markdownlint-cli2 applies ignores
# to command-line globs as well), so docs/archive/ stays excluded.
mapfile -d '' -t tracked_md < <(git ls-files -z -- '*.md')
if [ "${#tracked_md[@]}" -eq 0 ]; then
  echo "run-markdownlint: no tracked markdown files found." >&2
  exit 1
fi
"${lint_cmd[@]}" -- "${tracked_md[@]}"
