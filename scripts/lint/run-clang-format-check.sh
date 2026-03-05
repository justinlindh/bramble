#!/usr/bin/env bash
set -u

strict=0
mode="all"

for arg in "$@"; do
  case "$arg" in
    --strict)
      strict=1
      ;;
    --changed)
      mode="changed"
      ;;
  esac
done

if ! command -v clang-format >/dev/null 2>&1; then
  if (( strict )); then
    echo "[clang-format] FAIL: clang-format not found. Install clang-format to run strict check."
    exit 1
  fi
  echo "[clang-format] SKIP: clang-format not found. Install clang-format to enable this advisory check."
  exit 0
fi

if [[ "$mode" == "changed" ]]; then
  diff_range=""
  if [[ "${GITHUB_EVENT_NAME:-}" == "pull_request" && -n "${GITHUB_BASE_REF:-}" ]]; then
    git fetch origin "${GITHUB_BASE_REF}" >/dev/null 2>&1 || true
    base_ref="origin/${GITHUB_BASE_REF}"
    diff_range="$base_ref...HEAD"
  elif git rev-parse --verify HEAD~1 >/dev/null 2>&1; then
    diff_range="HEAD~1..HEAD"
  else
    git fetch origin main >/dev/null 2>&1 || true
    base_ref="origin/main"
    merge_base="$(git merge-base "$base_ref" HEAD 2>/dev/null || true)"
    if [[ -n "$merge_base" ]]; then
      diff_range="$merge_base...HEAD"
    fi
  fi

  if [[ -n "$diff_range" ]]; then
    mapfile -t files < <(git diff --name-only --diff-filter=ACMR "$diff_range" 2>/dev/null | grep -E '\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$' || true)
  else
    mapfile -t files < <(git ls-files \
      'main/**/*.c' 'main/**/*.h' \
      'components/**/*.c' 'components/**/*.h' \
      'test/**/*.c' 'test/**/*.h' \
      'simulator/**/*.c' 'simulator/**/*.h')
  fi
else
  mapfile -t files < <(git ls-files \
    'main/**/*.c' 'main/**/*.h' \
    'components/**/*.c' 'components/**/*.h' \
    'test/**/*.c' 'test/**/*.h' \
    'simulator/**/*.c' 'simulator/**/*.h')
fi

if [[ ${#files[@]} -eq 0 ]]; then
  echo "[clang-format] PASS: no tracked C/C header files in configured scope."
  exit 0
fi

tmp_out="$(mktemp)"
trap 'rm -f "$tmp_out"' EXIT

if ! clang-format --dry-run --Werror "${files[@]}" >"$tmp_out" 2>&1; then
  if (( strict )); then
    echo "[clang-format] FAIL: formatting differences detected (showing up to 200 lines)."
    sed -n '1,200p' "$tmp_out"
    exit 1
  fi
  echo "[clang-format] ADVISORY: formatting differences detected (showing up to 200 lines)."
  sed -n '1,200p' "$tmp_out"
  echo "[clang-format] Note: advisory mode, returning success."
  exit 0
fi

echo "[clang-format] PASS: formatting check clean for ${#files[@]} files."
exit 0
