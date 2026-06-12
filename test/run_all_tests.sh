#!/usr/bin/env bash
# Run all Bramble host tests.
# Fails (exit 1) if the build fails, if any suite fails, or if no suites are found.
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p build
cd build

echo "=== Configuring (cmake) ==="
if ! cmake .. > cmake.log 2>&1; then
    cat cmake.log
    echo "BUILD FAILED: cmake configure failed (full log above)" >&2
    exit 1
fi

echo "=== Building (make -j$(nproc)) ==="
if ! make -j"$(nproc)" > build.log 2>&1; then
    cat build.log
    echo "BUILD FAILED: make failed (full log above)" >&2
    exit 1
fi

TOTAL=0
PASS=0
FAIL=0
FAILED_SUITES=()

for test_bin in test_*; do
    if [ -f "$test_bin" ] && [ -x "$test_bin" ]; then
        echo "=== $test_bin ==="
        if "./$test_bin"; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
            FAILED_SUITES+=("$test_bin")
            echo "FAILED: $test_bin"
        fi
        TOTAL=$((TOTAL + 1))
    fi
done

echo ""
echo "================================"
echo "Test suites: $TOTAL total, $PASS passed, $FAIL failed"

if [ "$TOTAL" -eq 0 ]; then
    echo "NO TEST SUITES FOUND: build produced no test_* binaries ✗" >&2
    exit 1
fi

if [ "$FAIL" -ne 0 ]; then
    echo "SOME TESTS FAILED ✗"
    printf '  %s\n' "${FAILED_SUITES[@]}"
    exit 1
fi

echo "ALL TESTS PASSED ✓"
