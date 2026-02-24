#!/usr/bin/env bash
set -euo pipefail
if [ "$#" -lt 1 ]; then
  echo "Usage: $0 <file> [file ...]" >&2
  exit 1
fi
for f in "$@"; do
  [ -f "$f" ] || { echo "Missing file: $f" >&2; exit 1; }
  sha=$(sha256sum "$f" | awk '{print $1}')
  size=$(stat -c '%s' "$f")
  printf '%s\t%s\t%s\n' "$f" "$sha" "$size"
done
