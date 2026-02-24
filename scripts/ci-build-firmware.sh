#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT_DIR/build-artifacts/firmware-ci}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-ci-heltec-v3}"

log() {
  echo "[ci-build-firmware] $*"
}

die() {
  echo "[ci-build-firmware] ERROR: $*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing required command: $1"
}

require_file() {
  [[ -f "$1" ]] || die "Expected file not found: $1"
}

require_cmd idf.py
require_cmd sha256sum

cd "$ROOT_DIR"

log "Preparing deterministic output directory: $OUTPUT_DIR"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

log "Preparing clean build directory: $BUILD_DIR"
rm -rf "$BUILD_DIR"

log "Building firmware (Heltec V3 defaults via sdkconfig.defaults)"
idf.py \
  -B "$BUILD_DIR" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" \
  build

require_file "$BUILD_DIR/bramble.bin"
require_file "$BUILD_DIR/bramble.elf"
require_file "$BUILD_DIR/bramble.map"
require_file "$BUILD_DIR/bootloader/bootloader.bin"
require_file "$BUILD_DIR/partition_table/partition-table.bin"
require_file "$BUILD_DIR/ota_data_initial.bin"
require_file "$BUILD_DIR/flasher_args.json"

log "Collecting build artifacts"
cp "$BUILD_DIR/bramble.bin" "$OUTPUT_DIR/"
cp "$BUILD_DIR/bramble.elf" "$OUTPUT_DIR/"
cp "$BUILD_DIR/bramble.map" "$OUTPUT_DIR/"
cp "$BUILD_DIR/bootloader/bootloader.bin" "$OUTPUT_DIR/bootloader.bin"
cp "$BUILD_DIR/partition_table/partition-table.bin" "$OUTPUT_DIR/partition-table.bin"
cp "$BUILD_DIR/ota_data_initial.bin" "$OUTPUT_DIR/"
cp "$BUILD_DIR/flasher_args.json" "$OUTPUT_DIR/"

log "Writing checksums"
(
  cd "$OUTPUT_DIR"
  sha256sum \
    bramble.bin \
    bramble.elf \
    bramble.map \
    bootloader.bin \
    partition-table.bin \
    ota_data_initial.bin \
    flasher_args.json \
    > SHA256SUMS.txt
)

log "Done. Artifacts available at: $OUTPUT_DIR"
