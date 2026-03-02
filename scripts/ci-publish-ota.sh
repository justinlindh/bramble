#!/usr/bin/env bash
set -euo pipefail

is_valid_ota_semver() {
  local candidate=${1:-}
  [[ "$candidate" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.-]+)?$ ]]
}

normalize_version() {
  local version

  if [[ -n "${VERSION:-}" ]]; then
    version="$VERSION"
  else
    # Try latest firmware semver tag (set by semantic-release)
    version=$(git describe --tags --match 'firmware-v*' --always 2>/dev/null || true)
    # Strip the firmware- prefix: "firmware-v1.3.5-3-gabcdef" → "v1.3.5-3-gabcdef"
    version="${version#firmware-}"

    if [[ -z "$version" ]]; then
      version=$(git describe --tags --always --dirty 2>/dev/null || true)
    fi
  fi

  # Ensure leading v
  if [[ -n "$version" && ! "$version" =~ ^v ]]; then
    version="v${version}"
  fi

  # For dev channel, always emit a dev pre-release version.
  # - "v1.3.5-3-gabcdef" -> "v1.3.6-dev.3.gabcdef"
  # - "v1.3.5" (exact tag) -> "v1.3.6-dev.0.g<HEAD_SHA>"
  if [[ "${CHANNEL:-dev}" == "dev" ]]; then
    if [[ "$version" =~ ^(v[0-9]+\.[0-9]+\.)([0-9]+)-([0-9]+)-g([0-9a-f]+)$ ]]; then
      local prefix="${BASH_REMATCH[1]}"
      local patch="${BASH_REMATCH[2]}"
      local ahead="${BASH_REMATCH[3]}"
      local sha="${BASH_REMATCH[4]}"
      local next_patch=$((patch + 1))
      version="${prefix}${next_patch}-dev.${ahead}.g${sha}"
    elif [[ "$version" =~ ^(v[0-9]+\.[0-9]+\.)([0-9]+)$ ]]; then
      local prefix="${BASH_REMATCH[1]}"
      local patch="${BASH_REMATCH[2]}"
      local next_patch=$((patch + 1))
      local sha
      sha=$(git rev-parse --short HEAD)
      version="${prefix}${next_patch}-dev.0.g${sha}"
    fi
  fi

  if ! is_valid_ota_semver "$version"; then
    local short_sha
    short_sha=$(git rev-parse --short HEAD)
    version="v0.0.0-dev.${short_sha}"
  fi

  echo "$version"
}

if [[ "${1:-}" == "--print-version" ]]; then
  normalize_version
  exit 0
fi

if [[ "${1:-}" == "--set-version" ]]; then
  [[ -n "${2:-}" ]] || { echo "--set-version requires a value" >&2; exit 1; }
  VERSION="$2"
  shift 2
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
