#!/usr/bin/env bash
# Ruff baseline gate. `make ci-quality-ruff` and the Static checks job in
# .github/workflows/firmware-quality.yml both run this, so the ruff pin and the
# rule profile live only here.
set -u

# Must match the ruff baked into the CI runner image (bramble#222).
RUFF_VERSION="0.12.10"
RUFF_ARGS=(check scripts --select "E9,F63,F7,F82")

# Analysis roots are repo-relative, so run from the repo root regardless of the
# caller's working directory.
repo_root="$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
cd "$repo_root" || exit 1

found="none"
if command -v ruff >/dev/null 2>&1; then
  found="$(ruff --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
  if [[ "$found" == "$RUFF_VERSION" ]]; then
    exec ruff "${RUFF_ARGS[@]}"
  fi
fi

# A required CI gate must not depend on a PyPI download, so in CI a missing or
# mismatched ruff is runner-image drift and fails here rather than falling back.
if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then
  echo "::error::ruff on PATH is ${found:-unknown}, pinned version is ${RUFF_VERSION}. Align RUFF_VERSION in the runner-image definition with scripts/lint/run-ruff.sh."
  exit 1
fi

if command -v uvx >/dev/null 2>&1; then
  exec uvx --from "ruff==${RUFF_VERSION}" ruff "${RUFF_ARGS[@]}"
fi

echo "[ruff] FAIL: ruff on PATH is ${found:-unknown}, pinned version is ${RUFF_VERSION}, and uvx is not installed." >&2
echo "[ruff] Install uv (https://docs.astral.sh/uv/) to run the pinned ruff locally." >&2
exit 1
