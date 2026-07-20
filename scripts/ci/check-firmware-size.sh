#!/usr/bin/env bash
# Measure a freshly-built board's flash image and static DRAM, then ratchet-gate
# both against ci/size-baseline.json. Runs after scripts/flash.sh built the board
# into build-<board>/, with ESP-IDF already on PATH (the caller sources it).
#
#   scripts/ci/check-firmware-size.sh <board>
#
# flash_bytes  = size of build-<board>/bramble.bin (the flashed app image, the
#                number that must fit the OTA app partition)
# static_ram   = statically allocated data + bss from `idf.py size --format json`
#                (RAM reserved before the heap; the headroom issue #94 tracks)
#
# Env:
#   SIZE_ONLY_MEASURE=1  print "<board> <flash> <ram>" and skip the ratchet check
#                        (used by scripts/ci/update-size-baseline.sh)
set -euo pipefail

BOARD="${1:?usage: check-firmware-size.sh <board>}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build-$BOARD"
BIN="$BUILD_DIR/bramble.bin"

[ -f "$BIN" ] || { echo "::error::$BIN not found; build the board first" >&2; exit 1; }
command -v idf.py >/dev/null 2>&1 || { echo "::error::idf.py not on PATH; source ESP-IDF first" >&2; exit 1; }

FLASH_BYTES="$(stat -c %s "$BIN")"

# idf.py size reruns ninja (a no-op after the build) and prints build lines
# before the JSON; extract_static_ram.py finds and parses the trailing object.
SIZE_OUT="$(idf.py -C "$REPO_ROOT" -B "$BUILD_DIR" size --format json)"
STATIC_RAM="$(printf '%s\n' "$SIZE_OUT" | python3 "$REPO_ROOT/scripts/ci/extract_static_ram.py")"

echo "[size] $BOARD flash=$FLASH_BYTES B static_ram=$STATIC_RAM B"

if [ "${SIZE_ONLY_MEASURE:-0}" = "1" ]; then
    echo "$BOARD $FLASH_BYTES $STATIC_RAM"
    exit 0
fi

python3 "$REPO_ROOT/scripts/ci/check_size.py" "$BOARD" "$FLASH_BYTES" "$STATIC_RAM"
