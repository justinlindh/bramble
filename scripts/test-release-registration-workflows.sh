#!/usr/bin/env bash
set -euo pipefail

release_wf=".gitea/workflows/release-components.yml"
firmware_wf=".gitea/workflows/firmware-build.yml"
firmware_releaserc=".releaserc.firmware.cjs"
protocol_releaserc=".releaserc.protocol.cjs"
webapp_releaserc=".releaserc.webapp.cjs"
register_script="scripts/ci-register-gitea-release.sh"

rg -q '@saithodev/semantic-release-gitea' "$release_wf" || {
  echo "missing semantic-release gitea plugin install in $release_wf"
  exit 1
}

for rc in "$firmware_releaserc" "$protocol_releaserc" "$webapp_releaserc"; do
  rg -q '@saithodev/semantic-release-gitea' "$rc" || {
    echo "missing @saithodev/semantic-release-gitea plugin in $rc"
    exit 1
  }
done

rg -q 'Create or update Gitea Release' "$firmware_wf" || {
  echo "missing firmware release registration step in $firmware_wf"
  exit 1
}

rg -q 'scripts/ci-register-gitea-release.sh' "$firmware_wf" || {
  echo "missing release registration helper invocation in $firmware_wf"
  exit 1
}

[ -x "$register_script" ] || {
  echo "missing executable helper: $register_script"
  exit 1
}

rg -q '/api/v1/repos/' "$register_script" || {
  echo "missing Gitea API base usage in helper script"
  exit 1
}

rg -q 'releases/tags/' "$register_script" || {
  echo "missing Gitea release tag lookup usage in helper script"
  exit 1
}

rg -q '/assets' "$register_script" || {
  echo "missing release asset upload API usage in helper script"
  exit 1
}

echo "OK: release workflows are wired to create/update Gitea Releases with assets"
