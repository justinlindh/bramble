#!/usr/bin/env bash
set -euo pipefail

sourced=0
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  sourced=1
fi

# If idf.py is already on PATH, no explicit export script is required.
if command -v idf.py >/dev/null 2>&1; then
  echo "[ci-idf] idf.py already available on PATH"
  if (( sourced )); then return 0; else exit 0; fi
fi

candidates=()

if [[ -n "${IDF_PATH:-}" ]]; then
  candidates+=("$IDF_PATH/export.sh")
fi

candidates+=(
  "/opt/esp/idf/export.sh"
  "/opt/esp-idf/export.sh"
  "$HOME/src/esp-idf/export.sh"
  "$HOME/esp-idf/export.sh"
  "$HOME/esp/esp-idf/export.sh"
  "/usr/local/esp-idf/export.sh"
)

for export_sh in "${candidates[@]}"; do
  if [[ -f "$export_sh" ]]; then
    # shellcheck disable=SC1090
    source "$export_sh"
    if command -v idf.py >/dev/null 2>&1; then
      echo "[ci-idf] Loaded ESP-IDF from: $export_sh"
      if (( sourced )); then return 0; else exit 0; fi
    fi
  fi
done

echo "[ci-idf] ERROR: Unable to locate a working ESP-IDF export.sh; tried:" >&2
for export_sh in "${candidates[@]}"; do
  echo "  - $export_sh" >&2
done
if (( sourced )); then return 1; else exit 1; fi
