#!/usr/bin/env bash
set -euo pipefail

candidates=()

if [[ -n "${IDF_PATH:-}" ]]; then
  candidates+=("$IDF_PATH/export.sh")
fi

candidates+=(
  "/opt/esp/idf/export.sh"
  "$HOME/src/esp-idf/export.sh"
  "$HOME/esp/esp-idf/export.sh"
  "/usr/local/esp-idf/export.sh"
)

for export_sh in "${candidates[@]}"; do
  if [[ -f "$export_sh" ]]; then
    # shellcheck disable=SC1090
    source "$export_sh"
    if command -v idf.py >/dev/null 2>&1; then
      echo "[ci-idf] Loaded ESP-IDF from: $export_sh"
      exit 0
    fi
  fi
done

echo "[ci-idf] ERROR: Unable to locate a working ESP-IDF export.sh; tried:" >&2
for export_sh in "${candidates[@]}"; do
  echo "  - $export_sh" >&2
done
exit 1
