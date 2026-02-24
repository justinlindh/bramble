#!/usr/bin/env bash
set -euo pipefail

OTA_PUBLISH_URL=${OTA_PUBLISH_URL:-https://bramblemesh.org/ota/publish}
OTA_PUBLISH_KEY=${OTA_PUBLISH_KEY:-}
CHANNEL=${CHANNEL:-dev}
BOARD=${BOARD:-heltec-v3}
VERSION=${VERSION:-$(git describe --tags --always --dirty)}

BOOTLOADER=${BOOTLOADER:-build-artifacts/firmware-ci/bootloader.bin}
PARTITION=${PARTITION:-build-artifacts/firmware-ci/partition-table.bin}
FIRMWARE=${FIRMWARE:-build-artifacts/firmware-ci/bramble.bin}

[[ -n "$OTA_PUBLISH_KEY" ]] || { echo "OTA_PUBLISH_KEY is required" >&2; exit 1; }
for f in "$BOOTLOADER" "$PARTITION" "$FIRMWARE"; do
  [[ -f "$f" ]] || { echo "Missing artifact: $f" >&2; exit 1; }
done

resp_file=$(mktemp)
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

if [[ "$http_code" != "200" ]]; then
  echo "OTA publish failed (HTTP $http_code)" >&2
  cat "$resp_file" >&2
  exit 1
fi

cat "$resp_file"
if command -v jq >/dev/null 2>&1; then
  jq -e '.ok == true' "$resp_file" >/dev/null
fi

echo "OTA publish succeeded: version=$VERSION channel=$CHANNEL board=$BOARD"
