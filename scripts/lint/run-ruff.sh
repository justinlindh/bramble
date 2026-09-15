#!/usr/bin/env bash
# Single source of truth for the ruff baseline gate. Both
# `make ci-quality-ruff` and the Static checks job in
# .github/workflows/firmware-quality.yml invoke this script, so the pinned
# ruff version and the exact rule profile (the scanned roots plus the --select
# set) cannot drift between a local `make ci` and CI.
set -u

# The one place the ruff pin and the scanned profile live. The private runner
# image (bramble#222) bakes this same version; the assert below fails loud on
# image drift instead of silently linting with a different ruff.
RUFF_VERSION="0.12.10"
RUFF_ARGS=(check scripts --select "E9,F63,F7,F82")

# Analysis roots are repo-relative, so run from the repo root regardless of the
# caller's working directory.
repo_root="$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
cd "$repo_root" || exit 1

# RUFF_USE_UVX=1 fetches the pinned ruff from PyPI via uvx, for a local dev box
# with no ruff on PATH. CI never takes this path: the uvx download inside an
# always-run required gate is the startup-herd DNS failure the runner-image bake
# removed, so CI uses the image ruff and only asserts its version.
if [[ "${RUFF_USE_UVX:-0}" == "1" ]]; then
  if ! command -v uvx >/dev/null 2>&1; then
    echo "[ruff] FAIL: uvx not found. Install uv (https://docs.astral.sh/uv/) to run this gate locally." >&2
    exit 1
  fi
  exec uvx --from "ruff==${RUFF_VERSION}" ruff "${RUFF_ARGS[@]}"
fi

if ! command -v ruff >/dev/null 2>&1; then
  echo "[ruff] FAIL: ruff not found on PATH. Set RUFF_USE_UVX=1 to fetch the pinned ruff via uvx." >&2
  exit 1
fi

got="$(ruff --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
if [[ "$got" != "$RUFF_VERSION" ]]; then
  echo "::error::ruff version mismatch: runner image has ${got:-unknown}, pinned version is ${RUFF_VERSION}. Bump RUFF_VERSION in the private runner-image definition or this script so they agree."
  exit 1
fi

exec ruff "${RUFF_ARGS[@]}"
