#!/usr/bin/env bash
# Assemble the QEMU pager's merged flash + eFuse images from a firmware build,
# WITHOUT launching QEMU. Shared by run-qemu.sh (interactive boot) and the gosim
# supervisor's qemu-node path (emulator-qemu-mesh scenario), so both consume the
# exact same image-build logic. Idempotent: re-merges only when bramble.bin is
# newer than the merged image (or --fresh), re-seeds NVS only when the seed CSV
# changed. Prints the two image paths (flash, then eFuse) on stdout.
#
# Usage:
#   emulator/qemu/assemble-images.sh [--fresh]
#
# Env:
#   BUILD_DIR   firmware build dir (default: <repo>/build-qemu)
#   IDF_PATH    ESP-IDF (for nvs_partition_gen.py); source $IDF_PATH/export.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-qemu}"

FRESH=0
for arg in "$@"; do
    case "$arg" in
        --fresh) FRESH=1 ;;
        *) echo "assemble-images: unknown arg: $arg" >&2; exit 2 ;;
    esac
done

if [[ ! -f "$BUILD_DIR/bramble.bin" ]]; then
    echo "assemble-images: no $BUILD_DIR/bramble.bin; build the qemu variant first (see README.md)" >&2
    exit 1
fi

# Merged 8MB flash image. QEMU writes flash back to this file, so NVS (including
# node identity) persists across runs; --fresh regenerates it.
FLASH_IMG="$BUILD_DIR/flash_qemu.bin"
if [[ $FRESH -eq 1 || ! -f "$FLASH_IMG" || "$BUILD_DIR/bramble.bin" -nt "$FLASH_IMG" ]]; then
    echo "assemble-images: merging flash image..." >&2
    (cd "$BUILD_DIR" && python -m esptool --chip esp32s3 merge_bin \
        --output flash_qemu.bin --fill-flash-size 8MB \
        --flash_mode dio --flash_size 8MB --flash_freq 80m @flash_args)

    # Seed conn_mode=BLE (+ the shared mesh network key) into the freshly merged
    # image's NVS partition so boot skips the unmodeled WiFi PHY and the node
    # boots provisioned (see nvs-seed-ble.csv). QEMU-image-only: rewrites the
    # merged flash_qemu.bin at the nvs offset (0x9000, per partitions.csv),
    # never a real board, and only on a (re)merge.
    NVS_SEED="$BUILD_DIR/nvs_seed_ble.bin"
    NVS_CSV="$SCRIPT_DIR/nvs-seed-ble.csv"
    NVS_GEN="${IDF_PATH:-}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    if [[ ! -f "$NVS_SEED" || "$NVS_CSV" -nt "$NVS_SEED" ]]; then
        if [[ -f "$NVS_GEN" ]]; then
            python3 "$NVS_GEN" generate "$NVS_CSV" "$NVS_SEED" 0x5000 >/dev/null
        else
            echo "assemble-images: warning: nvs_partition_gen.py not found (source the IDF env);" \
                 "conn_mode/netkey seed NOT applied -- boot will wedge in the WiFi PHY" >&2
        fi
    fi
    if [[ -f "$NVS_SEED" ]]; then
        echo "assemble-images: seeding NVS (conn_mode=BLE + netkey) at 0x9000..." >&2
        dd if="$NVS_SEED" of="$FLASH_IMG" bs=1 seek=$((0x9000)) conv=notrunc status=none
    fi
fi

# eFuse image with ADC calib version set; without it boot wedges pre-app_main in
# ADC self-calibration (see README.md).
EFUSE_IMG="$BUILD_DIR/efuse_qemu.bin"
if [[ $FRESH -eq 1 || ! -f "$EFUSE_IMG" ]]; then
    python3 "$SCRIPT_DIR/mkefuse.py" "$EFUSE_IMG"
fi

echo "$FLASH_IMG"
echo "$EFUSE_IMG"
