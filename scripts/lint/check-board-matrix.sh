#!/usr/bin/env bash
# Assert that the CI board-build matrix covers every board the release build
# ships.
#
# scripts/ci-build-firmware.sh is the release path: its BOARDS list decides
# which targets get built, signed, and published. The board-build-smoke matrix
# in .github/workflows/quality.yml is the gate. If the two drift, a board can
# ship without any automatic build ever compiling it, which is exactly the hole
# this check exists to keep closed.
#
# Usage: bash scripts/lint/check-board-matrix.sh

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_SCRIPT="$ROOT_DIR/scripts/ci-build-firmware.sh"
WORKFLOW="$ROOT_DIR/.github/workflows/quality.yml"

fail() {
  echo "[board-matrix] FAIL: $*" >&2
  exit 1
}

[[ -f "$BUILD_SCRIPT" ]] || fail "missing $BUILD_SCRIPT"
[[ -f "$WORKFLOW" ]] || fail "missing $WORKFLOW"

# BOARDS=(heltec-v3 tdeck-plus heltec-v4 bramble-pager)
release_line="$(grep -m1 -E '^BOARDS=\(' "$BUILD_SCRIPT" || true)"
[[ -n "$release_line" ]] || fail "could not find a BOARDS=(...) line in $BUILD_SCRIPT"
release_boards="$(printf '%s\n' "$release_line" | sed -E 's/^BOARDS=\(//; s/\).*$//' | tr ' ' '\n' | grep -v '^$' | sort)"

#         board: [heltec-v3, tdeck-plus, heltec-v4, bramble-pager]
matrix_line="$(grep -m1 -E '^[[:space:]]+board:[[:space:]]*\[' "$WORKFLOW" || true)"
[[ -n "$matrix_line" ]] || fail "could not find a 'board: [...]' matrix line in $WORKFLOW"
matrix_boards="$(printf '%s\n' "$matrix_line" | sed -E 's/^[^[]*\[//; s/\].*$//' | tr ',' '\n' | tr -d ' "' | grep -v '^$' | sort)"

if [[ "$release_boards" != "$matrix_boards" ]]; then
  echo "[board-matrix] Release build boards (scripts/ci-build-firmware.sh):" >&2
  printf '%s\n' "$release_boards" | sed 's/^/  /' >&2
  echo "[board-matrix] CI smoke matrix boards (.github/workflows/quality.yml):" >&2
  printf '%s\n' "$matrix_boards" | sed 's/^/  /' >&2
  fail "the board-build-smoke matrix does not match the release board list; every shipped board must be built by CI"
fi

count="$(printf '%s\n' "$release_boards" | wc -l | tr -d ' ')"
echo "[board-matrix] clean: $count boards built by both the release path and the CI smoke matrix"
