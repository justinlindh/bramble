#!/usr/bin/env bash
set -euo pipefail

wf=".gitea/workflows/firmware-build.yml"
build_script="scripts/ci-build-firmware.sh"

for board in heltec-v3 tdeck-plus heltec-v4; do
  rg -q "$board" "$wf" || { echo "missing board in workflow: $board"; exit 1; }
  rg -q "$board" "$build_script" || { echo "missing board in build script: $board"; exit 1; }
done

echo "OK: board matrix references present"
