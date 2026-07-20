#!/usr/bin/env bash
# Required gate: markdownlint-cli2 over every tracked markdown file, using the
# root .markdownlint-cli2.yaml config (globs and ignores live there, not here).
# There is no advisory mode: this script's predecessor defaulted to reporting
# findings and exiting 0, which docs/quality-policy.md forbids, and it was
# deleted rather than wired into CI for exactly that reason (issue #160). A
# real gate needed the tree reformatted first; that sweep landed alongside
# this script.
#
# markdownlint-cli2 is baked into the CI runner image (bramble#222, image
# 1.2.0, installed globally at build time as root; the job user could never
# `npm install -g` into the image's prefix, which is why the job-time global
# install was rejected before the bake). When the PATH binary is present its
# version must MATCH the pin below: a silently different linter version is
# image drift, and drift fails loud here rather than producing findings (or
# passes) that no local run can reproduce. Environments without the bake fall
# back to the same pinned version via npx. The package's declared engines
# (node >=22) versus the image's node 20 is an EBADENGINE warning only,
# verified to run correctly.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

MARKDOWNLINT_CLI2_VERSION="0.23.1"

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
  if [ "${found_version}" != "${MARKDOWNLINT_CLI2_VERSION}" ]; then
    echo "run-markdownlint: markdownlint-cli2 on PATH is ${found_version}, pinned version is ${MARKDOWNLINT_CLI2_VERSION}." >&2
    echo "run-markdownlint: fix the drift: align the private runner-image bake or this pin." >&2
    exit 1
  fi
  lint_cmd=(markdownlint-cli2)
elif command -v npx >/dev/null 2>&1; then
  lint_cmd=(npx --yes "markdownlint-cli2@${MARKDOWNLINT_CLI2_VERSION}")
else
  echo "run-markdownlint: neither markdownlint-cli2 nor npx found on PATH." >&2
  echo "Install Node, then run: npx --yes markdownlint-cli2@${MARKDOWNLINT_CLI2_VERSION}" >&2
  exit 1
fi

# Lint exactly the TRACKED markdown files, which is what this gate has always
# promised (see the header). Relying on the config's repo-wide `**/*.md` glob
# instead meant every UNTRACKED markdown on disk was linted too, so a clean-CI
# tree could fail locally on gitignored build output (webapp/dist/), agent
# worktrees, or any scratch file, teaching people to distrust the documented
# local command. Passing the tracked list as CLI globs disables the config's
# `globs:` but keeps its `ignores:` (markdownlint-cli2 applies ignores to
# command-line globs as well), so docs/archive/ stays excluded in both modes.
mapfile -d '' -t tracked_md < <(git ls-files -z -- '*.md')
if [ "${#tracked_md[@]}" -eq 0 ]; then
  echo "run-markdownlint: no tracked markdown files found." >&2
  exit 1
fi
"${lint_cmd[@]}" -- "${tracked_md[@]}"
