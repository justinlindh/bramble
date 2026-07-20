#!/usr/bin/env bash
# Deliberately rebuild every board and rewrite the ceilings in
# ci/size-baseline.json. Run this by hand when a change legitimately grows the
# flash image or static RAM and you have justified it in the PR. The file is
# committed, so the baseline only ever moves in a reviewed diff, never on its
# own. ESP-IDF must be on PATH (source scripts/ci-source-idf.sh first); sizes are
# toolchain-sensitive, so the canonical numbers are what CI measures on the
# pinned IDF, and when this script's local numbers differ, prefer the CI values.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASELINE="$REPO_ROOT/ci/size-baseline.json"
BOARDS=(heltec-v3 tdeck-plus heltec-v4 bramble-pager)

command -v idf.py >/dev/null 2>&1 || { echo "source ESP-IDF first (scripts/ci-source-idf.sh)" >&2; exit 1; }
export BRAMBLE_OTA_ALLOW_GENERATED_KEY="${BRAMBLE_OTA_ALLOW_GENERATED_KEY:-1}"

declare -A FLASH RAM
for board in "${BOARDS[@]}"; do
    echo "=== building $board ==="
    bash "$REPO_ROOT/scripts/flash.sh" local "$board" build >/dev/null
    read -r _ f r < <(SIZE_ONLY_MEASURE=1 bash "$REPO_ROOT/scripts/ci/check-firmware-size.sh" "$board" | tail -1)
    FLASH[$board]="$f"
    RAM[$board]="$r"
    echo "$board flash=$f static_ram=$r"
done

python3 - "$BASELINE" "${BOARDS[@]}" <<PY
import json, sys
path = sys.argv[1]
boards = sys.argv[2:]
flash = { $(for b in "${BOARDS[@]}"; do printf "'%s': %s, " "$b" "${FLASH[$b]}"; done) }
ram = { $(for b in "${BOARDS[@]}"; do printf "'%s': %s, " "$b" "${RAM[$b]}"; done) }
with open(path, encoding="utf-8") as fh:
    data = json.load(fh)
for b in boards:
    data["boards"][b]["flash_bytes"] = flash[b]
    data["boards"][b]["static_ram_bytes"] = ram[b]
with open(path, "w", encoding="utf-8") as fh:
    json.dump(data, fh, indent=2)
    fh.write("\n")
print("wrote", path)
PY
echo "Review the diff and commit ci/size-baseline.json deliberately."
