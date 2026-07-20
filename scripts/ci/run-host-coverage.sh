#!/usr/bin/env bash
# Measure host-test line coverage and ratchet-gate it against the committed
# baseline. Builds the Unity host suite a second time with gcov instrumentation
# (BRAMBLE_COVERAGE=ON, ASan off) into test/build-coverage/, runs every binary so
# gcov emits .gcda, aggregates line coverage over product code (components/,
# main/) with scripts/ci/host_coverage.py, then checks it with
# scripts/ci/check_coverage.py.
#
# The pass/fail of the tests themselves is NOT gated here; that is the pristine
# ASan `Host tests` run. Here a binary that exits non-zero still produced its
# coverage data, so we only need it to execute.
#
# Env:
#   COVERAGE_ONLY_MEASURE=1  print the percentage and skip the ratchet check
#                            (used by scripts/ci/update-coverage-baseline.sh)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/test/build-coverage"

echo "=== Configuring coverage build (BRAMBLE_COVERAGE=ON) ==="
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cmake -S "$REPO_ROOT/test" -B "$BUILD_DIR" -DBRAMBLE_COVERAGE=ON > "$BUILD_DIR/cmake.log" 2>&1 || {
    cat "$BUILD_DIR/cmake.log"
    echo "COVERAGE BUILD FAILED: cmake configure failed" >&2
    exit 1
}

echo "=== Building coverage binaries (make -j) ==="
make -C "$BUILD_DIR" -j"$(nproc)" > "$BUILD_DIR/build.log" 2>&1 || {
    cat "$BUILD_DIR/build.log"
    echo "COVERAGE BUILD FAILED: make failed" >&2
    exit 1
}

echo "=== Running instrumented binaries to emit .gcda ==="
ran=0
# Leak detection needs ptrace; the coverage run does not care about leaks and
# must not depend on the pods' ptrace capability, so disable it.
export ASAN_OPTIONS="detect_leaks=0:${ASAN_OPTIONS:-}"
for bin in "$BUILD_DIR"/test_*; do
    [ -x "$bin" ] || continue
    # A failing assertion still wrote coverage data; ignore the exit status.
    "$bin" > /dev/null 2>&1 || true
    ran=$((ran + 1))
done
echo "ran $ran instrumented binaries"
if [ "$ran" -eq 0 ]; then
    echo "COVERAGE FAILED: no test_* binaries executed" >&2
    exit 1
fi

echo "=== Aggregating coverage (gcov) ==="
PCT="$(python3 "$REPO_ROOT/scripts/ci/host_coverage.py" "$BUILD_DIR" \
    --repo-root "$REPO_ROOT" \
    --json "$BUILD_DIR/host-coverage.json" | tail -1)"
echo "measured host-c line coverage: ${PCT}%"

if [ "${COVERAGE_ONLY_MEASURE:-0}" = "1" ]; then
    echo "$PCT"
    exit 0
fi

echo "=== Ratchet check ==="
python3 "$REPO_ROOT/scripts/ci/check_coverage.py" host-c "$PCT"
