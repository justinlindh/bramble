#!/usr/bin/env bash
set -euo pipefail

if command -v idf.py >/dev/null 2>&1; then
  echo "[ci-idf] idf.py already available on PATH"
  exit 0
fi

if [[ -f "$HOME/esp-idf/export.sh" ]]; then
  echo "[ci-idf] Reusing existing ESP-IDF at $HOME/esp-idf"
  exit 0
fi

echo "[ci-idf] Installing ESP-IDF v5.4.1 to $HOME/esp-idf"
git clone --depth 1 --branch v5.4.1 https://github.com/espressif/esp-idf.git "$HOME/esp-idf"
"$HOME/esp-idf/install.sh" esp32
