#!/usr/bin/env bash
# Run the QEMU-variant pager image in qemu-system-xtensa (esp32s3 machine).
#
# Prerequisites (see README.md):
#   - build-qemu/ built via the sdkconfig.defaults.qemu layering
#   - qemu-system-xtensa from espressif/qemu esp-develop-9.2.2 or newer
#     (9.0.0, the version ESP-IDF v5.4 pins, crashes on SHA-over-GDMA)
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

# Merged 8MB flash image. QEMU writes flash back to this file, so NVS
# (including the node identity) persists across runs; --fresh regenerates it.
FLASH_IMG="$BUILD_DIR/flash_qemu.bin"
if [[ $FRESH -eq 1 || ! -f "$FLASH_IMG" || "$BUILD_DIR/bramble.bin" -nt "$FLASH_IMG" ]]; then
    echo "merging flash image..."
    (cd "$BUILD_DIR" && python -m esptool --chip esp32s3 merge_bin \
        --output flash_qemu.bin --fill-flash-size 8MB \
        --flash_mode dio --flash_size 8MB --flash_freq 80m @flash_args)

    # Seed conn_mode=BLE into the freshly merged image's NVS partition so boot
    # skips the unmodeled WiFi PHY and reaches the main loop (see
    # nvs-seed-ble.csv). QEMU-image-only: this rewrites the merged flash_qemu.bin
    # at the nvs offset (0x9000, per partitions.csv), never a real board, and
    # only on a (re)merge -- runtime NVS writes from prior runs are untouched
    # because a reused image is not re-merged.
    NVS_SEED="$BUILD_DIR/nvs_seed_ble.bin"
    NVS_CSV="$SCRIPT_DIR/nvs-seed-ble.csv"
    NVS_GEN="${IDF_PATH:-}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    if [[ ! -f "$NVS_SEED" || "$NVS_CSV" -nt "$NVS_SEED" ]]; then
        if [[ -f "$NVS_GEN" ]]; then
            python3 "$NVS_GEN" generate "$NVS_CSV" "$NVS_SEED" 0x5000 >/dev/null
        else
            echo "warning: nvs_partition_gen.py not found (source the IDF env);" \
                 "conn_mode=BLE seed NOT applied -- boot will wedge in the WiFi PHY" >&2
        fi
    fi
    if [[ -f "$NVS_SEED" ]]; then
        echo "seeding conn_mode=BLE into QEMU image NVS (0x9000)..."
        dd if="$NVS_SEED" of="$FLASH_IMG" bs=1 seek=$((0x9000)) conv=notrunc status=none
    fi
fi

# eFuse image with ADC calib version set; without it boot wedges pre-app_main
# in ADC self-calibration (see README.md).
EFUSE_IMG="$BUILD_DIR/efuse_qemu.bin"
if [[ ! -f "$EFUSE_IMG" ]]; then
    python3 "$SCRIPT_DIR/mkefuse.py" "$EFUSE_IMG"
fi

exec "$QEMU_XTENSA" -machine esp32s3 -nographic \
    -drive file="$FLASH_IMG",if=mtd,format=raw \
    -drive file="$EFUSE_IMG",if=none,format=raw,id=efuse \
    -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
    "${GDB_ARGS[@]}"
