#!/bin/bash
# Run all Bramble host tests
cd "$(dirname "$0")/build"
cmake .. > /dev/null 2>&1
make -j$(nproc) > /dev/null 2>&1

TOTAL=0
PASS=0
FAIL=0

for test_bin in test_*; do
    if [ -x "$test_bin" ]; then
        echo "=== $test_bin ==="
        if ./$test_bin; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
            echo "FAILED: $test_bin"
        fi
        TOTAL=$((TOTAL + 1))
    fi
done

echo ""
echo "================================"
echo "Test suites: $TOTAL total, $PASS passed, $FAIL failed"
if [ $FAIL -eq 0 ]; then
    echo "ALL TESTS PASSED ✓"
else
    echo "SOME TESTS FAILED ✗"
    exit 1
fi
