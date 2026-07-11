#!/usr/bin/env bash
# Task 1 gate evidence: assert the linux-target firmware boots app_main ->
# mesh_task, attempts the first beacon, and idles without crashing or
# spinning a core. Build first: idf.py --preview set-target linux && idf.py
# build. Optional arg: path to the binary (default build/bramble-node.elf).
set -euo pipefail
cd "$(dirname "$0")"

BIN="${1:-build/bramble-node.elf}"
if [ ! -x "$BIN" ]; then
    echo "FAIL: $BIN not found; run idf.py build first"
    exit 1
fi

LOG="$(mktemp)"
PID=""
cleanup() {
    if [ -n "$PID" ]; then
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -f "$LOG"
}
trap cleanup EXIT

"$BIN" >"$LOG" 2>&1 &
PID=$!

# Boot plus the first-beacon window (beacon jitter is a few seconds).
sleep 15

if ! kill -0 "$PID" 2>/dev/null; then
    echo "FAIL: node process exited during boot; last log lines:"
    tail -20 "$LOG"
    exit 1
fi

fail=0
for pat in \
    "BOOT STAGE: app_main entry" \
    "Node address:" \
    "null radio up" \
    "BOOT STAGE: mesh_task start" \
    "BOOT STAGE: sending first beacon" \
    "BOOT STAGE: entering main mesh loop" \
    "BOOT STAGE: main loop start"; do
    if ! grep -q "$pat" "$LOG"; then
        echo "FAIL: missing boot milestone: $pat"
        fail=1
    fi
done
if [ "$fail" -ne 0 ]; then
    tail -20 "$LOG"
    exit 1
fi

# Near-idle check: cumulative CPU share since spawn, well after boot. A
# busy-wait that never yields would show close to 100 here.
CPU="$(ps -p "$PID" -o %cpu --no-headers | tr -d ' ')"
if ! awk -v c="$CPU" 'BEGIN { exit (c + 0 < 20.0) ? 0 : 1 }'; then
    echo "FAIL: node is not idle (${CPU}% CPU)"
    exit 1
fi

echo "PASS: booted app_main -> mesh_task, first beacon attempted, idling at ${CPU}% CPU"
