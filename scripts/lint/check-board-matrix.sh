#!/usr/bin/env bash
# Assert that every place that enumerates the shipped boards agrees with the
# release board list.
#
# scripts/ci-build-firmware.sh is the release path: its BOARDS list decides
# which targets get built, signed, and published. Three other sites hardcode
# the same set and each one silently breaks a different board if it drifts:
#   - the board-build-smoke matrix in .github/workflows/quality.yml, the gate
#     that compiles every target (drift there ships a board no CI build ever
#     compiled),
#   - scripts/flash.sh is_board(), the accept-list a bench flash is checked
#     against (drift there rejects a real board as "not a board"),
#   - scripts/flash-fleet.sh, which rebuilds every board image before a fleet
#     flash (drift there leaves a board with no freshly built image, so
#     flash-fleet.sh skips every node of that type).
# This check keeps all four in lockstep so adding a board to the release path
# without teaching the other three fails here instead of at a bench.
#
# Usage: bash scripts/lint/check-board-matrix.sh

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_SCRIPT="$ROOT_DIR/scripts/ci-build-firmware.sh"
WORKFLOW="$ROOT_DIR/.github/workflows/quality.yml"
FLASH_SCRIPT="$ROOT_DIR/scripts/flash.sh"
FLEET_SCRIPT="$ROOT_DIR/scripts/flash-fleet.sh"

fail() {
  echo "[board-matrix] FAIL: $*" >&2
  exit 1
}

for f in "$BUILD_SCRIPT" "$WORKFLOW" "$FLASH_SCRIPT" "$FLEET_SCRIPT"; do
  [[ -f "$f" ]] || fail "missing $f"
done

# BOARDS=(heltec-v3 tdeck-plus heltec-v4 bramble-pager)
release_line="$(grep -m1 -E '^BOARDS=\(' "$BUILD_SCRIPT" || true)"
[[ -n "$release_line" ]] || fail "could not find a BOARDS=(...) line in $BUILD_SCRIPT"
release_boards="$(printf '%s\n' "$release_line" | sed -E 's/^BOARDS=\(//; s/\).*$//' | tr ' ' '\n' | grep -v '^$' | sort)"

#         board: [heltec-v3, tdeck-plus, heltec-v4, bramble-pager]
matrix_line="$(grep -m1 -E '^[[:space:]]+board:[[:space:]]*\[' "$WORKFLOW" || true)"
[[ -n "$matrix_line" ]] || fail "could not find a 'board: [...]' matrix line in $WORKFLOW"
matrix_boards="$(printf '%s\n' "$matrix_line" | sed -E 's/^[^[]*\[//; s/\].*$//' | tr ',' '\n' | tr -d ' "' | grep -v '^$' | sort)"

# is_board() { [[ "$1" == "heltec-v3" || "$1" == "heltec-v4" || ... ]]; }
flash_line="$(grep -m1 -E '^is_board\(\)' "$FLASH_SCRIPT" || true)"
[[ -n "$flash_line" ]] || fail "could not find an is_board() definition in $FLASH_SCRIPT"
# Every board name is a double-quoted literal; "$1" is not (the $ excludes it).
flash_boards="$(printf '%s\n' "$flash_line" | grep -oE '"[a-z0-9-]+"' | tr -d '"' | sort)"
[[ -n "$flash_boards" ]] || fail "could not extract any board names from is_board() in $FLASH_SCRIPT"

#   for board in heltec-v3 heltec-v4 tdeck-plus bramble-pager; do
fleet_line="$(grep -m1 -E '^[[:space:]]*for board in ' "$FLEET_SCRIPT" || true)"
[[ -n "$fleet_line" ]] || fail "could not find a 'for board in ...' build loop in $FLEET_SCRIPT"
fleet_boards="$(printf '%s\n' "$fleet_line" | sed -E 's/^[[:space:]]*for board in //; s/;.*$//' | tr ' ' '\n' | grep -v '^$' | sort)"

# Compare one site's board set against the release list, printing both on drift.
check_against_release() {
  local label="$1" boards="$2"
  if [[ "$release_boards" != "$boards" ]]; then
    echo "[board-matrix] Release build boards (scripts/ci-build-firmware.sh):" >&2
    printf '%s\n' "$release_boards" | sed 's/^/  /' >&2
    echo "[board-matrix] ${label}:" >&2
    printf '%s\n' "$boards" | sed 's/^/  /' >&2
    fail "${label} does not match the release board list; every shipped board must be listed here too"
  fi
}

check_against_release "CI smoke matrix boards (.github/workflows/quality.yml)" "$matrix_boards"
check_against_release "flash.sh is_board() accept-list" "$flash_boards"
check_against_release "flash-fleet.sh build loop" "$fleet_boards"

count="$(printf '%s\n' "$release_boards" | wc -l | tr -d ' ')"
echo "[board-matrix] clean: $count boards agree across the release path, the CI smoke matrix, flash.sh, and flash-fleet.sh"
