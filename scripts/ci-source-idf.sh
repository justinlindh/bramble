#!/usr/bin/env bash
set -euo pipefail

_ci_source_idf_finish() {
  local code="$1"
  if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    return "$code"
  fi
  exit "$code"
}

if command -v idf.py >/dev/null 2>&1; then
  echo "[ci-idf] idf.py already available on PATH"
  _ci_source_idf_finish 0
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
      _ci_source_idf_finish 0
    fi
  fi
done

echo "[ci-idf] ERROR: Unable to locate a working ESP-IDF export.sh; tried:" >&2
for export_sh in "${candidates[@]}"; do
  echo "  - $export_sh" >&2
done
_ci_source_idf_finish 1
