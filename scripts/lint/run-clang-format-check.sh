#!/usr/bin/env bash
set -u

if ! command -v clang-format >/dev/null 2>&1; then
  echo "[clang-format] SKIP: clang-format not found. Install clang-format to enable this advisory check."
  exit 0
fi

mapfile -t files < <(git ls-files \
  'main/**/*.c' 'main/**/*.h' \
  'components/**/*.c' 'components/**/*.h' \
  'test/**/*.c' 'test/**/*.h' \
  'simulator/**/*.c' 'simulator/**/*.h')

if [[ ${#files[@]} -eq 0 ]]; then
  echo "[clang-format] PASS: no tracked C/C header files in configured scope."
  exit 0
fi

tmp_out="$(mktemp)"
trap 'rm -f "$tmp_out"' EXIT

if ! clang-format --dry-run --Werror "${files[@]}" >"$tmp_out" 2>&1; then
  echo "[clang-format] ADVISORY: formatting differences detected (showing up to 200 lines)."
  sed -n '1,200p' "$tmp_out"
  echo "[clang-format] Note: advisory mode, returning success."
  exit 0
fi

echo "[clang-format] PASS: formatting check clean for ${#files[@]} files."
exit 0
