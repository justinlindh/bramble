#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 4 ]; then
  echo "Usage: $0 <ota-root> <channel> <version> <board=file> [board=file ...]" >&2
  exit 1
fi

OTA_ROOT="$1"; shift
CHANNEL="$1"; shift
VERSION="$1"; shift

mkdir -p "$OTA_ROOT/$CHANNEL/$VERSION"
PUBLISHED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf '{"published_at":"%s"}\n' "$PUBLISHED_AT" > "$OTA_ROOT/$CHANNEL/$VERSION/release-meta.json"

for pair in "$@"; do
  board="${pair%%=*}"
  file="${pair#*=}"
  [ -f "$file" ] || { echo "Missing artifact: $file" >&2; exit 1; }
  destDir="$OTA_ROOT/$CHANNEL/$VERSION/$board"
  mkdir -p "$destDir"
  cp "$file" "$destDir/"
  dest="$destDir/$(basename "$file")"
  sha=$(sha256sum "$dest" | awk '{print $1}')
  size=$(stat -c '%s' "$dest")
  printf '{"sha256":"%s","size":%s}\n' "$sha" "$size" > "$dest.meta.json"
done

dir="$(cd "$(dirname "$0")" && pwd)"
node "$dir/build-firmware-index.js" "$OTA_ROOT" "$OTA_ROOT/index.json"
