#!/usr/bin/env bash
set -u

if ! command -v shellcheck >/dev/null 2>&1; then
  echo "[shellcheck] SKIP: shellcheck not found. Install shellcheck to enable this advisory check."
  exit 0
fi

mapfile -t files < <(git ls-files '*.sh')

if [[ ${#files[@]} -eq 0 ]]; then
  echo "[shellcheck] PASS: no tracked shell scripts found."
  exit 0
fi

tmp_out="$(mktemp)"
trap 'rm -f "$tmp_out"' EXIT

if ! shellcheck "${files[@]}" >"$tmp_out" 2>&1; then
  echo "[shellcheck] ADVISORY: findings detected (showing up to 250 lines)."
  sed -n '1,250p' "$tmp_out"
  echo "[shellcheck] Note: advisory mode, returning success."
  exit 0
fi

echo "[shellcheck] PASS: ${#files[@]} scripts checked with no findings."
exit 0
