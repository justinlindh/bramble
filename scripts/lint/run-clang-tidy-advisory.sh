#!/usr/bin/env bash
set -u

if ! command -v clang-tidy >/dev/null 2>&1; then
  echo "[clang-tidy] SKIP: clang-tidy not found. Install clang-tidy to enable this advisory check."
  exit 0
fi

build_dir="${1:-build}"
compile_db="${build_dir}/compile_commands.json"

if [[ ! -f "$compile_db" ]]; then
  echo "[clang-tidy] SKIP: $compile_db not found. Run a firmware build first to generate compile commands."
  exit 0
fi

# Single-star globs, not DIR/**/*.c: git pathspec wildcards already span '/',
# so 'main/*.c' matches every .c under main/ at any depth, while 'main/**/*.c'
# requires an intervening directory and silently skips files directly under
# main/ (all 18, plus 118 top-level test/ sources). Matches the pattern list in
# run-clang-format-check.sh. Do not "fix" to **.
mapfile -t files < <(git ls-files \
  'main/*.c' \
  'components/*.c' \
  'test/*.c' \
  'simulator/*.c')

if [[ ${#files[@]} -eq 0 ]]; then
  echo "[clang-tidy] PASS: no tracked C sources in configured scope."
  exit 0
fi

tmp_out="$(mktemp)"
trap 'rm -f "$tmp_out"' EXIT

max_files=20
scanned=0
for f in "${files[@]}"; do
  ((scanned++))
  if (( scanned > max_files )); then
    break
  fi
  clang-tidy -p "$build_dir" "$f" >>"$tmp_out" 2>&1 || true

  if grep -q "unknown target triple 'xtensa-esp32s3-unknown-elf'" "$tmp_out"; then
    echo "[clang-tidy] SKIP: local clang-tidy cannot parse ESP-IDF xtensa compile flags from $compile_db."
    echo "[clang-tidy] Advisory note: run clang-tidy in an environment with xtensa-capable LLVM or host-target compile commands."
    exit 0
  fi
done

if [[ -s "$tmp_out" ]]; then
  echo "[clang-tidy] ADVISORY: findings detected in first $(( scanned < max_files ? scanned : max_files )) files (showing up to 250 lines)."
  sed -n '1,250p' "$tmp_out"
  echo "[clang-tidy] Note: advisory mode, returning success."
  exit 0
fi

echo "[clang-tidy] PASS: no findings in scanned file subset."
exit 0
