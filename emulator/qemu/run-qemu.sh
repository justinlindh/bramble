#!/usr/bin/env bash
# Run the QEMU-variant pager image in qemu-system-xtensa (esp32s3 machine).
#
# Prerequisites (see README.md):
#   - build-qemu/ built via the sdkconfig.defaults.qemu layering
#   - qemu-system-xtensa from espressif/qemu esp-develop-9.2.2 or newer
#     (9.0.0, the version ESP-IDF v5.4.1 pins, crashes on SHA-over-GDMA)
#
# Usage:
#   ./run-qemu.sh            boot with UART0 on stdio
#   ./run-qemu.sh --gdb      freeze at reset with a gdbserver on :1234
#   ./run-qemu.sh --fresh    discard flash state (NVS identity) from prior runs
#
# Env:
#   QEMU_XTENSA   path to qemu-system-xtensa (default: the from-source build
#                 at $QEMU_SRC/build/qemu-system-xtensa if present, else
#                 first hit in PATH; see bootstrap-qemu.sh)
#   QEMU_SRC      from-source QEMU tree (default: ~/src/qemu-esp)
#   BUILD_DIR     firmware build dir (default: <repo>/build-qemu)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-qemu}"
QEMU_SRC="${QEMU_SRC:-$HOME/src/qemu-esp}"
if [[ -z "${QEMU_XTENSA:-}" && -x "$QEMU_SRC/build/qemu-system-xtensa" ]]; then
    QEMU_XTENSA="$QEMU_SRC/build/qemu-system-xtensa"
fi
QEMU_XTENSA="${QEMU_XTENSA:-$(command -v qemu-system-xtensa || true)}"

GDB_ARGS=()
FRESH=0
for arg in "$@"; do
    case "$arg" in
        --gdb) GDB_ARGS=(-s -S) ;;
        --fresh) FRESH=1 ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

if [[ -z "$QEMU_XTENSA" ]]; then
    echo "qemu-system-xtensa not found; set QEMU_XTENSA (see README.md)" >&2
    exit 1
fi
if [[ ! -f "$BUILD_DIR/bramble.bin" ]]; then
    echo "no $BUILD_DIR/bramble.bin; build the qemu variant first (see README.md)" >&2
    exit 1
fi

# Merged 8MB flash image + eFuse image, assembled by the shared script (also
# used by the gosim qemu-node path). QEMU writes flash back to FLASH_IMG so NVS
# (including node identity) persists across runs; --fresh regenerates both.
ASSEMBLE_ARGS=()
[[ $FRESH -eq 1 ]] && ASSEMBLE_ARGS=(--fresh)
mapfile -t _imgs < <(BUILD_DIR="$BUILD_DIR" bash "$SCRIPT_DIR/assemble-images.sh" "${ASSEMBLE_ARGS[@]}")
FLASH_IMG="${_imgs[0]}"
EFUSE_IMG="${_imgs[1]}"

exec "$QEMU_XTENSA" -machine esp32s3 -nographic \
    -drive file="$FLASH_IMG",if=mtd,format=raw \
    -drive file="$EFUSE_IMG",if=none,format=raw,id=efuse \
    -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
    "${GDB_ARGS[@]}"
