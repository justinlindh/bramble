#!/usr/bin/env bash
set -euo pipefail

is_valid_ota_semver() {
  local candidate=${1:-}
  [[ "$candidate" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.-]+)?$ ]]
}

normalize_version() {
  local version
  local runtime_version_file=${RPC_VERSION_FILE:-main/rpc_methods.c}
  local strict_source=${STRICT_VERSION_SOURCE:-}

  if [[ -n "${VERSION:-}" ]]; then
    version="$VERSION"
  else
    # Primary source: firmware runtime version constant
    version=$(sed -n 's/^#define BRAMBLE_VERSION_STR[[:space:]]*"\([^"]\+\)".*/\1/p' "$runtime_version_file" 2>/dev/null | head -n1)

    if [[ -z "$version" ]]; then
      if [[ -n "$strict_source" || -n "${CI:-}" || -n "${GITHUB_ACTIONS:-}" ]]; then
        echo "ERROR: BRAMBLE_VERSION_STR is missing or unreadable from $runtime_version_file" >&2
        return 1
      fi
      # Non-CI fallback when tags are available
      version=$(git describe --tags --always --dirty 2>/dev/null || true)
    fi
  fi

  # Ensure leading v for OTA release identity
  if [[ -n "$version" && ! "$version" =~ ^v ]]; then
    version="v${version}"
  fi

  if ! is_valid_ota_semver "$version"; then
    if [[ -z "${VERSION:-}" && ( -n "$strict_source" || -n "${CI:-}" || -n "${GITHUB_ACTIONS:-}" ) ]]; then
      echo "ERROR: Invalid BRAMBLE_VERSION_STR semver: '$version'" >&2
      return 1
    fi

    local short_sha
    short_sha=$(git rev-parse --short HEAD)
    version="v0.0.0-${short_sha}"
  fi

  echo "$version"
}

if [[ "${1:-}" == "--print-version" ]]; then
  normalize_version
  exit 0
fi

OTA_PUBLISH_URL=${OTA_PUBLISH_URL:-https://bramblemesh.org/ota/publish}
OTA_PUBLISH_KEY=${OTA_PUBLISH_KEY:-}
CHANNEL=${CHANNEL:-dev}
BOARD=${BOARD:-heltec-v3}
VERSION=$(normalize_version)

BOOTLOADER=${BOOTLOADER:-build-artifacts/firmware-ci/bootloader.bin}
PARTITION=${PARTITION:-build-artifacts/firmware-ci/partition-table.bin}
FIRMWARE=${FIRMWARE:-build-artifacts/firmware-ci/bramble.bin}

[[ -n "$OTA_PUBLISH_KEY" ]] || { echo "OTA_PUBLISH_KEY is required" >&2; exit 1; }
for f in "$BOOTLOADER" "$PARTITION" "$FIRMWARE"; do
  [[ -f "$f" ]] || { echo "Missing artifact: $f" >&2; exit 1; }
done

resp_file=$(mktemp)
trap 'rm -f "$resp_file"' EXIT
http_code=$(curl -sS -o "$resp_file" -w "%{http_code}" -X POST "$OTA_PUBLISH_URL" \
  -H "Authorization: Bearer $OTA_PUBLISH_KEY" \
  -F "version=$VERSION" \
  -F "channel=$CHANNEL" \
  -F "board=$BOARD" \
  -F "commit=$(git rev-parse --short HEAD)" \
  -F "run_id=${GITHUB_RUN_ID:-unknown}" \
  -F "bootloader.bin=@$BOOTLOADER" \
  -F "partition-table.bin=@$PARTITION" \
  -F "bramble.bin=@$FIRMWARE")

if [[ ! "$http_code" =~ ^2[0-9][0-9]$ ]]; then
  echo "OTA publish failed (HTTP $http_code)" >&2
  cat "$resp_file" >&2
  exit 1
fi

cat "$resp_file"
if command -v jq >/dev/null 2>&1; then
  jq -e '.ok == true' "$resp_file" >/dev/null
fi

echo "OTA publish succeeded: version=$VERSION channel=$CHANNEL board=$BOARD"
