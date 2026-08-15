#!/usr/bin/env bash
set -u

strict=0
if [[ "${1:-}" == "--strict" ]]; then
  strict=1
fi

if ! command -v shellcheck >/dev/null 2>&1; then
  if (( strict )); then
    echo "[shellcheck] FAIL: shellcheck not found. Install shellcheck to run strict check."
    exit 1
  fi
  echo "[shellcheck] SKIP: shellcheck not found. Install shellcheck to enable this advisory check."
  exit 0
fi

if (( strict )); then
  # Keep required gate on a low-noise, actively maintained subset. scripts/lib
  # holds helpers the flash paths source before a flash-or-brick decision, so
  # it is held to the same strict bar, along with its fixture tests.
  mapfile -t files < <(git ls-files 'scripts/lint/*.sh' 'scripts/lib/*.sh' 'scripts/test-crypt-state.sh')
else
  # Broader visibility in advisory mode.
  mapfile -t files < <(git ls-files '*.sh')
fi

if [[ ${#files[@]} -eq 0 ]]; then
  echo "[shellcheck] PASS: no tracked shell scripts found."
  exit 0
fi

tmp_out="$(mktemp)"
trap 'rm -f "$tmp_out"' EXIT

if ! shellcheck "${files[@]}" >"$tmp_out" 2>&1; then
  if (( strict )); then
    echo "[shellcheck] FAIL: findings detected (showing up to 250 lines)."
    sed -n '1,250p' "$tmp_out"
    exit 1
  fi
  echo "[shellcheck] ADVISORY: findings detected (showing up to 250 lines)."
  sed -n '1,250p' "$tmp_out"
  echo "[shellcheck] Note: advisory mode, returning success."
  exit 0
fi

echo "[shellcheck] PASS: ${#files[@]} scripts checked with no findings."
exit 0
