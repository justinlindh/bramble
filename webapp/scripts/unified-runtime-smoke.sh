#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${1:-http://127.0.0.1:8085}"

check_endpoint() {
  local path="$1"
  local url="${BASE_URL}${path}"

  echo "==> GET ${url}"
  local response
  response="$(curl -sS -i "$url")"
  printf '%s\n\n' "$response"
}

check_endpoint "/"
check_endpoint "/api/healthz"
check_endpoint "/api/mode"
