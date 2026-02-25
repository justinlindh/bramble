#!/usr/bin/env bash
set -euo pipefail

release_wf=".gitea/workflows/release-components.yml"
firmware_wf=".gitea/workflows/firmware-build.yml"
publish_script="scripts/ci-publish-ota.sh"

# release-components must include firmware semantic-release job.
rg -q '^\s*release-firmware:' "$release_wf" || {
  echo "missing release-firmware job in release-components workflow"
  exit 1
}

rg -q 'semantic-release --extends \./\.releaserc\.firmware\.cjs' "$release_wf" || {
  echo "release-firmware job must run semantic-release with .releaserc.firmware.cjs"
  exit 1
}

# firmware build must be triggered by firmware source changes (not just CI script edits).
for required_path in "main/**" "components/**" "partitions.csv" "sdkconfig.defaults"; do
  rg -Fq -- "- '$required_path'" "$firmware_wf" || {
    echo "firmware-build workflow missing push path trigger: $required_path"
    exit 1
  }
done

# Publish script must support explicit version injection from semantic-release.
rg -Fq 'if [[ "${1:-}" == "--print-version" ]]' "$publish_script" || {
  echo "publish script missing --print-version support"
  exit 1
}
rg -Fq 'if [[ "${1:-}" == "--set-version" ]]' "$publish_script" || {
  echo "publish script missing --set-version support for semantic-release handoff"
  exit 1
}

echo "OK: firmware release CI contract satisfied"
