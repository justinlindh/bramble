#!/usr/bin/env bash
# This helper must be SOURCED, never executed: it exports ESP-IDF into the
# caller's shell (`source scripts/ci-source-idf.sh`), so a subprocess run would
# discard the very environment it sets up. Every caller sources it, so it exits
# with `return`; running it directly errors out, which is the intended signal.
set -euo pipefail

# If idf.py is already on PATH, no explicit export script is required.
if command -v idf.py >/dev/null 2>&1; then
  echo "[ci-idf] idf.py already available on PATH"
  return 0
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
      return 0
    fi
  fi
done

echo "[ci-idf] ERROR: Unable to locate a working ESP-IDF export.sh; tried:" >&2
for export_sh in "${candidates[@]}"; do
  echo "  - $export_sh" >&2
done
return 1
