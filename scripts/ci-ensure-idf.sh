#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PERSIST_GITHUB_ENV=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --github-env)
      PERSIST_GITHUB_ENV=1
      shift
      ;;
    *)
      echo "[ci-idf] ERROR: unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

persist_to_github_env() {
  if [[ -z "${GITHUB_ENV:-}" || -z "${GITHUB_PATH:-}" ]]; then
    echo "[ci-idf] ERROR: --github-env requested but GITHUB_ENV/GITHUB_PATH is not set" >&2
    return 1
  fi

  local key
  for key in IDF_PATH IDF_TOOLS_PATH IDF_PYTHON_ENV_PATH ESP_IDF_VERSION; do
    if [[ -n "${!key:-}" ]]; then
      printf '%s=%s\n' "$key" "${!key}" >> "$GITHUB_ENV"
    fi
  done

  local entry
  IFS=':' read -r -a before_parts <<< "${PATH_BEFORE_SOURCE:-}"
  IFS=':' read -r -a after_parts <<< "$PATH"

  for entry in "${after_parts[@]}"; do
    [[ -z "$entry" ]] && continue

    local seen=0
    local prev
    for prev in "${before_parts[@]}"; do
      if [[ "$entry" == "$prev" ]]; then
        seen=1
        break
      fi
    done

    if [[ "$seen" -eq 0 ]]; then
      printf '%s\n' "$entry" >> "$GITHUB_PATH"
      before_parts+=("$entry")
    fi
  done

  echo "[ci-idf] Persisted ESP-IDF env vars to GITHUB_ENV and PATH additions to GITHUB_PATH"
}

echo "[ci-idf] CI assert mode: expecting ESP-IDF to be pre-baked on this runner (no runtime install)"

PATH_BEFORE_SOURCE="$PATH"
# First, try to source ESP-IDF from known locations via shared helper.
# shellcheck disable=SC1090,SC1091
if source "$SCRIPT_DIR/ci-source-idf.sh"; then
  echo "[ci-idf] OK: ESP-IDF toolchain is available"
  idf.py --version || true
  python3 --version || true
  if [[ "$PERSIST_GITHUB_ENV" -eq 1 ]]; then
    persist_to_github_env
  fi
  exit 0
fi

# Fallback diagnostics in case helper exits unexpectedly without clear detail.
echo "[ci-idf] ERROR: ESP-IDF toolchain is missing on this runner." >&2
echo "[ci-idf] This CI pipeline requires a pre-baked ESP-IDF environment (idf-node label)." >&2
echo "[ci-idf] Checked via scripts/ci-source-idf.sh and idf.py was not available." >&2
echo "[ci-idf] Action: re-route this job to an idf-node runner or pre-install ESP-IDF v5.4.1 and tools." >&2
exit 1
