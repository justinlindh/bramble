#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT_DIR/build-artifacts/firmware-ci}"

BOARDS=(heltec-v3 tdeck-plus heltec-v4)

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

board_defaults() {
  case "$1" in
    heltec-v3) echo "sdkconfig.defaults" ;;
    tdeck-plus) echo "sdkconfig.defaults;sdkconfig.defaults.tdeck_plus" ;;
    heltec-v4) echo "sdkconfig.defaults;sdkconfig.defaults.heltec_v4" ;;
    *) die "Unsupported board: $1" ;;
  esac
}

require_cmd idf.py
require_cmd sha256sum

cd "$ROOT_DIR"

log "Preparing deterministic output directory: $OUTPUT_DIR"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

for board in "${BOARDS[@]}"; do
  BUILD_DIR="$ROOT_DIR/build-ci-$board"
  BOARD_OUT_DIR="$OUTPUT_DIR/$board"
  DEFAULTS="$(board_defaults "$board")"

  log "Preparing clean build directory: $BUILD_DIR"
  rm -rf "$BUILD_DIR"

  BOARD_SDKCONFIG="$ROOT_DIR/sdkconfig.$board"
  rm -f "$BOARD_SDKCONFIG"

  log "Building firmware for $board (SDKCONFIG=$BOARD_SDKCONFIG DEFAULTS=$DEFAULTS)"
  idf.py \
    -B "$BUILD_DIR" \
    -D SDKCONFIG="$BOARD_SDKCONFIG" \
    -D SDKCONFIG_DEFAULTS="$DEFAULTS" \
    build

  require_file "$BUILD_DIR/bramble.bin"
  require_file "$BUILD_DIR/bramble.elf"
  require_file "$BUILD_DIR/bramble.map"
  require_file "$BUILD_DIR/bootloader/bootloader.bin"
  require_file "$BUILD_DIR/partition_table/partition-table.bin"
  require_file "$BUILD_DIR/ota_data_initial.bin"
  require_file "$BUILD_DIR/flasher_args.json"

  mkdir -p "$BOARD_OUT_DIR"

  log "Collecting build artifacts for $board"
  cp "$BUILD_DIR/bramble.bin" "$BOARD_OUT_DIR/bramble.bin"
  cp "$BUILD_DIR/bramble.elf" "$BOARD_OUT_DIR/bramble.elf"
  cp "$BUILD_DIR/bramble.map" "$BOARD_OUT_DIR/bramble.map"
  cp "$BUILD_DIR/bootloader/bootloader.bin" "$BOARD_OUT_DIR/bootloader.bin"
  cp "$BUILD_DIR/partition_table/partition-table.bin" "$BOARD_OUT_DIR/partition-table.bin"
  cp "$BUILD_DIR/ota_data_initial.bin" "$BOARD_OUT_DIR/ota_data_initial.bin"
  cp "$BUILD_DIR/flasher_args.json" "$BOARD_OUT_DIR/flasher_args.json"

  log "Writing checksums for $board"
  (
    cd "$BOARD_OUT_DIR"
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
done

log "Done. Artifacts available at: $OUTPUT_DIR"