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

# Verification must use real index artifact fields (.file/.path), not synthetic .filename.
rg -q '\.file' "$wf" || { echo "missing .file-based artifact verification in workflow"; exit 1; }
rg -q '\.path' "$wf" || { echo "missing .path-based artifact verification in workflow"; exit 1; }
if rg -q '\.filename' "$wf"; then
  echo "workflow must not rely on synthetic .filename field"
  exit 1
fi

# Canonical artifacts must be enforced for each required board.
for artifact in 'bootloader.bin' 'partition-table.bin' 'bramble.bin'; do
  rg -q "$artifact" "$wf" || { echo "missing canonical artifact check in workflow: $artifact"; exit 1; }
done

rg -q 'all\(\$boards\[\]; \$release \| has_required_for_board\(\.\)\)' "$wf" || {
  echo "missing all-boards completeness gate in workflow"
  exit 1
}

rg -q 'normalize_version\(\)' "$publish_script" || {
  echo "missing normalize_version helper in publish script"
  exit 1
}

echo "OK: board matrix + index-field checks + per-board canonical artifact completeness present"
