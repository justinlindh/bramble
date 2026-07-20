#!/usr/bin/env bash
# Measure a freshly-built board's app flash footprint and static DRAM, then
# ratchet-gate both against ci/size-baseline.json. Runs after scripts/flash.sh
# built the board into build-<board>/, with ESP-IDF already on PATH (the caller
# sources it).
#
#   scripts/ci/check-firmware-size.sh <board>
#
# flash_bytes  = app flash footprint (flash_code + flash_rodata + flash_other)
#                from `idf.py size --format json`. This is the UNPADDED figure;
#                bramble.bin is padded up to 64 KiB ESP32-S3 MMU pages, which
#                makes the image size jump a whole page at a boundary and differ
#                by 64 KiB between ESP-IDF patch versions, so the raw image size
#                is not a precise regression metric. Partition fit is already
#                enforced by the ESP-IDF build.
# static_ram   = statically allocated data + bss (RAM reserved before the heap;
#                the headroom issue #94 tracks)
#
# Env:
#   SIZE_ONLY_MEASURE=1  print "<board> <flash> <ram>" and skip the ratchet check
#                        (used by scripts/ci/update-size-baseline.sh)
set -euo pipefail

BOARD="${1:?usage: check-firmware-size.sh <board>}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build-$BOARD"

[ -d "$BUILD_DIR" ] || { echo "::error::$BUILD_DIR not found; build the board first" >&2; exit 1; }
command -v idf.py >/dev/null 2>&1 || { echo "::error::idf.py not on PATH; source ESP-IDF first" >&2; exit 1; }

# idf.py size reruns ninja (a no-op after the build) and prints build lines
# before the JSON; extract_firmware_sizes.py finds the trailing object and emits
# "<app_flash_bytes> <static_ram_bytes>".
SIZE_OUT="$(idf.py -C "$REPO_ROOT" -B "$BUILD_DIR" size --format json)"
read -r FLASH_BYTES STATIC_RAM < <(printf '%s\n' "$SIZE_OUT" | python3 "$REPO_ROOT/scripts/ci/extract_firmware_sizes.py")

echo "[size] $BOARD flash=$FLASH_BYTES B static_ram=$STATIC_RAM B"

if [ "${SIZE_ONLY_MEASURE:-0}" = "1" ]; then
    echo "$BOARD $FLASH_BYTES $STATIC_RAM"
    exit 0
fi

python3 "$REPO_ROOT/scripts/ci/check_size.py" "$BOARD" "$FLASH_BYTES" "$STATIC_RAM"
