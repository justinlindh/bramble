#!/usr/bin/env bash
set -euo pipefail

wf=".gitea/workflows/firmware-build.yml"
build_script="scripts/ci-build-firmware.sh"
publish_script="scripts/ci-publish-ota.sh"

for board in heltec-v3 tdeck-plus heltec-v4; do
  rg -q "$board" "$wf" || { echo "missing board in workflow: $board"; exit 1; }
  rg -q "$board" "$build_script" || { echo "missing board in build script: $board"; exit 1; }
done

rg -q 'scripts/ci-publish-ota.sh --print-version' "$wf" || {
  echo "missing shared version normalization call in workflow verification"
  exit 1
}

for artifact in 'bootloader.bin' 'partition-table.bin' 'bramble.bin'; do
  rg -q "$artifact" "$wf" || { echo "missing canonical artifact check in workflow: $artifact"; exit 1; }
done

rg -q 'normalize_version\(\)' "$publish_script" || {
  echo "missing normalize_version helper in publish script"
  exit 1
}

echo "OK: board matrix + version normalization + canonical artifact checks present"
