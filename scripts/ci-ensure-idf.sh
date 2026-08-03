#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "[ci-idf] CI assert mode: expecting ESP-IDF to be pre-baked on this runner (no runtime install)"

# Source ESP-IDF from known locations via the shared helper.
# shellcheck disable=SC1090,SC1091
if source "$SCRIPT_DIR/ci-source-idf.sh"; then
  echo "[ci-idf] OK: ESP-IDF toolchain is available"
  idf.py --version || true
  python3 --version || true
  exit 0
fi

# Fallback diagnostics in case helper exits unexpectedly without clear detail.
echo "[ci-idf] ERROR: ESP-IDF toolchain is missing on this runner." >&2
echo "[ci-idf] This CI pipeline requires a pre-baked ESP-IDF environment (idf-node label)." >&2
echo "[ci-idf] Checked via scripts/ci-source-idf.sh and idf.py was not available." >&2
echo "[ci-idf] Action: re-route this job to an idf-node runner or pre-install ESP-IDF v5.4.1 and tools." >&2
exit 1
