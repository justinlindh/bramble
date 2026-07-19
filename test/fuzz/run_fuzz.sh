#!/usr/bin/env bash
# Build and run the Bramble libFuzzer harnesses over the committed corpora.
#
# Default budget is FUZZ_SECONDS (30s) per target, two targets, so a full run
# is about a minute of fuzzing plus a few seconds of compilation. That is the
# CI shape: short, bounded, and gating.
#
#   bash test/fuzz/run_fuzz.sh                  # the CI run
#   FUZZ_SECONDS=300 bash test/fuzz/run_fuzz.sh # a longer local campaign
#   bash test/fuzz/run_fuzz.sh --regen-corpus   # rebuild the committed seeds
#
# A crash, timeout, or OOM leaves the reproducing input in
# test/fuzz/artifacts/ and fails the script. Feed it back with:
#
#   test/fuzz/build/fuzz_packet test/fuzz/artifacts/crash-<hash>
#
set -euo pipefail

FUZZ_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$FUZZ_DIR/../.." && pwd)"
BUILD_DIR="$FUZZ_DIR/build"
CORPUS_DIR="$FUZZ_DIR/corpus"
ARTIFACT_DIR="$FUZZ_DIR/artifacts"

FUZZ_SECONDS="${FUZZ_SECONDS:-30}"
FUZZ_CC="${FUZZ_CC:-clang}"

regen=0
for arg in "$@"; do
  case "$arg" in
    --regen-corpus) regen=1 ;;
    *)
      echo "unknown argument: $arg" >&2
      exit 2
      ;;
  esac
done

if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
  echo "FAIL: $FUZZ_CC not found. The fuzz harnesses need clang with libFuzzer" >&2
  echo "      (-fsanitize=fuzzer); gcc does not ship it. Install clang or set" >&2
  echo "      FUZZ_CC to a clang that does." >&2
  exit 1
fi

# Fail on a clang that was built without the libFuzzer runtime rather than
# letting the missing archive surface as an unrelated-looking link error.
probe="$(mktemp -d)"
trap 'rm -rf "$probe"' EXIT
cat >"$probe/probe.c" <<'EOF'
#include <stddef.h>
#include <stdint.h>
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t s) { (void)d; (void)s; return 0; }
EOF
if ! "$FUZZ_CC" -fsanitize=fuzzer,address -o "$probe/probe" "$probe/probe.c" >"$probe/log" 2>&1; then
  echo "FAIL: $FUZZ_CC cannot link -fsanitize=fuzzer,address. libFuzzer and the" >&2
  echo "      ASan runtime must be installed alongside clang (Debian/Ubuntu:" >&2
  echo "      the clang and libclang-rt-dev packages)." >&2
  sed -n '1,20p' "$probe/log" >&2
  exit 1
fi

CFLAGS=(
  -std=c11
  -g
  -O1
  -Wall -Wextra -Werror
  "-fsanitize=fuzzer,address,undefined"
  -fno-omit-frame-pointer
  -fno-sanitize-recover=undefined
  -I"$REPO_ROOT/components/packet/include"
  -I"$REPO_ROOT/components/fragment/include"
  -I"$REPO_ROOT/test/stubs"
)

mkdir -p "$BUILD_DIR" "$ARTIFACT_DIR"

echo "=== Building fuzz targets ($("$FUZZ_CC" --version | head -1)) ==="
"$FUZZ_CC" "${CFLAGS[@]}" -o "$BUILD_DIR/fuzz_packet" \
  "$FUZZ_DIR/fuzz_packet.c" "$REPO_ROOT/components/packet/packet.c"
"$FUZZ_CC" "${CFLAGS[@]}" -o "$BUILD_DIR/fuzz_fragment" \
  "$FUZZ_DIR/fuzz_fragment.c" "$REPO_ROOT/components/fragment/fragment.c"

if (( regen )); then
  echo "=== Regenerating seed corpora ==="
  mkdir -p "$CORPUS_DIR/packet" "$CORPUS_DIR/fragment"
  # No fuzzer runtime here: gen_corpus has its own main().
  "$FUZZ_CC" -std=c11 -g -O1 -Wall -Wextra -Werror \
    -I"$REPO_ROOT/components/packet/include" \
    -I"$REPO_ROOT/components/fragment/include" \
    -I"$REPO_ROOT/test/stubs" \
    -o "$BUILD_DIR/gen_corpus" \
    "$FUZZ_DIR/gen_corpus.c" \
    "$REPO_ROOT/components/packet/packet.c" \
    "$REPO_ROOT/components/fragment/fragment.c"
  "$BUILD_DIR/gen_corpus" "$CORPUS_DIR"
  echo "Corpus regenerated. Review and commit the diff under $CORPUS_DIR."
  exit 0
fi

status=0
for target in packet fragment; do
  seeds="$CORPUS_DIR/$target"
  if [ ! -d "$seeds" ] || [ -z "$(ls -A "$seeds" 2>/dev/null)" ]; then
    echo "FAIL: seed corpus $seeds is missing or empty." >&2
    echo "      Regenerate it with: bash test/fuzz/run_fuzz.sh --regen-corpus" >&2
    exit 1
  fi

  # libFuzzer writes newly discovered inputs into the FIRST corpus directory
  # on its command line. Point that at a scratch directory so a run never
  # mutates the committed seeds, which stay a read-only second directory.
  work="$(mktemp -d)"

  echo ""
  echo "=== Fuzzing $target for ${FUZZ_SECONDS}s ==="
  # -max_total_time is the whole budget; run count is left unbounded.
  # -detect_leaks=0: LeakSanitizer needs ptrace, which the container runners
  # only grant through the setpriv wrapper in quality.yml, and no function
  # under test allocates, so there is nothing here for LSan to find.
  if ! "$BUILD_DIR/fuzz_$target" \
    -max_total_time="$FUZZ_SECONDS" \
    -max_len=1024 \
    -timeout=10 \
    -rss_limit_mb=2048 \
    -print_final_stats=1 \
    -artifact_prefix="$ARTIFACT_DIR/" \
    -detect_leaks=0 \
    "$work" "$seeds"; then
    echo "FAIL: fuzz_$target reported a finding. Reproducer saved under $ARTIFACT_DIR/" >&2
    status=1
  fi
  rm -rf "$work"
done

echo ""
if [ "$status" -ne 0 ]; then
  echo "FUZZING FAILED: see the reproducers in $ARTIFACT_DIR/" >&2
  exit 1
fi

echo "FUZZING CLEAN: no findings in ${FUZZ_SECONDS}s per target"
