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

# MAX_FILES limits the scan for a quick spot check; the default scans the
# whole candidate set.
if [[ -n "${MAX_FILES:-}" ]] && (( MAX_FILES < ${#files[@]} )); then
  files=("${files[@]:0:MAX_FILES}")
fi

tmp_dir="$(mktemp -d)"
tmp_out="$(mktemp)"
trap 'rm -rf "$tmp_dir"; rm -f "$tmp_out"' EXIT

# The IDF build drives gcc, and its compile commands carry gcc-only codegen
# flags that clang rejects as hard errors on every file, drowning real
# findings. Scan against a sanitized copy of the compile database with those
# flags stripped. Needs jq; without it, fall back to the raw database.
scan_db_dir="$build_dir"
if command -v jq >/dev/null 2>&1; then
  mkdir "$tmp_dir/db"
  jq '[.[] | if has("command") then
        .command |= gsub(" -(mlongcalls|fno-shrink-wrap|fno-tree-switch-conversion|fstrict-volatile-bitfields) "; " ")
      else . end]' "$compile_db" >"$tmp_dir/db/compile_commands.json"
  scan_db_dir="$tmp_dir/db"
else
  echo "[clang-tidy] NOTE: jq not found; scanning against the raw compile database (gcc-only flags may add noise)."
fi

# Findings go to stdout; stderr only carries the "N warnings generated" /
# "Suppressed N warnings" stats boilerplate, which would make every file
# look like it has findings, so stderr is dropped (probe excepted).
# Probe with the first file serially: a clang-tidy that cannot parse the
# ESP-IDF xtensa compile flags at all fails identically on every file, so
# detect that before fanning out across the full set.
clang-tidy -p "$scan_db_dir" "${files[0]}" >"$tmp_dir/000000.out" 2>"$tmp_dir/probe.err" || true
if grep -q "unknown target triple 'xtensa-esp32s3-unknown-elf'" "$tmp_dir/probe.err" "$tmp_dir/000000.out"; then
  echo "[clang-tidy] SKIP: local clang-tidy cannot parse ESP-IDF xtensa compile flags from $compile_db."
  echo "[clang-tidy] Advisory note: run clang-tidy in an environment with xtensa-capable LLVM or host-target compile commands."
  exit 0
fi

# Scan the rest in parallel, one output file per source keyed by a
# zero-padded input index so the aggregate stays in input order.
jobs="$(nproc)"
export CT_SCAN_DB_DIR="$scan_db_dir" CT_TMP_DIR="$tmp_dir"
if (( ${#files[@]} > 1 )); then
  # shellcheck disable=SC2016  # single quotes intended: expansion happens in the child bash, from the exported CT_* env
  for i in "${!files[@]}"; do
    (( i == 0 )) && continue
    printf '%06d %s\0' "$i" "${files[$i]}"
  done | xargs -0 -P "$jobs" -n 1 bash -c \
    'idx="${0%% *}"; f="${0#* }"; clang-tidy -p "$CT_SCAN_DB_DIR" "$f" >"$CT_TMP_DIR/$idx.out" 2>/dev/null || true'

fi

with_findings=0
for out in "$tmp_dir"/*.out; do
  if [[ -s "$out" ]]; then
    with_findings=$((with_findings + 1))
    cat "$out" >>"$tmp_out"
  fi
done

if [[ -s "$tmp_out" ]]; then
  echo "[clang-tidy] ADVISORY: findings in $with_findings of ${#files[@]} scanned files (showing up to 250 lines)."
  sed -n '1,250p' "$tmp_out"
  echo "[clang-tidy] Note: advisory mode, returning success."
  exit 0
fi

echo "[clang-tidy] PASS: no findings in ${#files[@]} scanned files."
exit 0
