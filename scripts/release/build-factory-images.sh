#!/usr/bin/env bash
# Build plain factory images for the off-the-shelf Bramble boards and merge
# each into a single esptool-flashable binary, then emit a SHA256SUMS manifest.
#
# These are factory images for INITIAL serial (esptool) flashing only. The
# production OTA signing key and the OTA update channel are deliberately kept
# out: the build runs with BRAMBLE_OTA_ALLOW_GENERATED_KEY=1 so every app is
# signed with a throwaway key, never the real OTA_SIGNING_KEY secret. Devices
# flashed with these images receive real OTA updates through the separately
# built, properly signed channel, not from anything produced here.
#
# The pager is skipped on purpose (board not fabbed).
#
# Usage: build-factory-images.sh <version> [outdir]
#   version  release version without the leading "v" (e.g. 1.6.0)
#   outdir   directory for the merged .bin files + SHA256SUMS (default dist/factory)
set -euo pipefail

VERSION="${1:?usage: build-factory-images.sh <version> [outdir]}"
OUTDIR_ARG="${2:-dist/factory}"

BOARDS=(heltec-v3 heltec-v4 tdeck-plus)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

# Throwaway signing key only: keep the production OTA key out of factory images.
export BRAMBLE_OTA_ALLOW_GENERATED_KEY=1

# shellcheck source=/dev/null
source scripts/ci-source-idf.sh

mkdir -p "$OUTDIR_ARG"
OUTDIR="$(cd "$OUTDIR_ARG" && pwd)"

echo "==> Building factory images for version $VERSION"
echo "==> Output directory: $OUTDIR"

for board in "${BOARDS[@]}"; do
  echo
  echo "==> [$board] build start"
  start=$(date +%s)

  bash scripts/flash.sh local "$board" build

  build_dir="build-${board}"
  if [[ ! -f "$build_dir/flash_args" ]]; then
    echo "ERROR: $build_dir/flash_args missing after build" >&2
    exit 1
  fi

  # Read the target chip from the build's own flasher_args.json rather than
  # hardcoding it; offsets and flash settings come from the build's flash_args.
  # Bare "python" (not python3) is deliberate here and below: it resolves to
  # the shim inside the ESP-IDF venv sourced above, matching how idf.py
  # invokes esptool.
  chip="$(python -c "import json,sys; print(json.load(open('$build_dir/flasher_args.json'))['extra_esptool_args']['chip'])")"

  out="$OUTDIR/bramble_${VERSION}_${board}_factory.bin"
  # esptool expands @flash_args into the offset/file pairs and flash settings
  # the build recorded; run from the build dir so the relative paths resolve.
  ( cd "$build_dir" && python -m esptool --chip "$chip" merge_bin -o "$out" @flash_args )

  end=$(date +%s)
  echo "==> [$board] done in $((end - start))s -> $(basename "$out") ($(stat -c%s "$out") bytes)"
done

echo
echo "==> Writing SHA256SUMS"
( cd "$OUTDIR" && sha256sum bramble_"${VERSION}"_*_factory.bin > SHA256SUMS )

echo "==> Factory artifacts:"
ls -l "$OUTDIR"
cat "$OUTDIR/SHA256SUMS"
