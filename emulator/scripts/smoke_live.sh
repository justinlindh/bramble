#!/usr/bin/env bash
#
# smoke_live.sh: end-to-end live-mesh smoke test for the Bramble emulator.
#
# Boots the gosim broker headless with the emulator-3-pagers scenario, which
# spawns three real-firmware virtual pager processes (the IDF linux node
# binary), and asserts the live integration:
#
#   (a) three external firmware nodes attach with distinct node ids (hello);
#   (c) each node emits at least one framebuffer (fb) message (boot screen);
#   (b) transmit pricing/delivery -- see ACCEPTANCE LEVEL below;
#   (d) persistence: killing a node process makes the supervisor restart it and
#       the SAME node id re-attaches (identity survived via $NODE_DIR/flash.bin).
#
# ACCEPTANCE LEVEL: attach + hello + fb + persistence (a, c, d).
# Level (b) full beacon delivery is NOT reached: the firmware boots UNPROVISIONED
# (no network key) and is therefore INERT -- the beacon path runs but emits no
# TX by design (emulator/node/README.md). There is no emu-link provisioning
# message in protocol v1, so the three pagers never key up and never transmit.
# This is the documented maximum for this task; the script asserts a,c,d and
# reports the observed TX count (expected 0) for (b).
#
# PREREQUISITES (this script builds nothing):
#   1. The node binary:   cd emulator/node && idf.py build
#      (after `source $IDF_PATH/export.sh; idf.py --preview set-target linux`)
#      produces emulator/node/build/bramble-node.elf
#   2. The gosim binary:  cd simulator/gosim && go build -o bramble-gosim .
#
# Exit code 0 on PASS, non-zero on any failed assertion.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

NODE_BIN="emulator/node/build/bramble-node.elf"
GOSIM_BIN="simulator/gosim/bramble-gosim"
SCENARIO="simulator/scenarios/emulator-3-pagers.json"

LOG="$(mktemp -t smoke_live.XXXXXX.log)"
GOSIM_PID=""

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '  %s\n' "$*"; }

cleanup() {
    if [ -n "$GOSIM_PID" ] && kill -0 "$GOSIM_PID" 2>/dev/null; then
        # Kill the node children first (direct children of gosim), then gosim.
        pkill -P "$GOSIM_PID" 2>/dev/null || true
        kill "$GOSIM_PID" 2>/dev/null || true
        wait "$GOSIM_PID" 2>/dev/null || true
    fi
    # Safety net for any orphaned node processes from this run.
    pkill -f "$NODE_BIN" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { red "FAIL: $*"; echo "--- last 40 log lines ---"; tail -40 "$LOG"; exit 1; }

# --- preconditions --------------------------------------------------------
[ -x "$NODE_BIN" ]  || fail "node binary missing/not executable: $NODE_BIN (build it: cd emulator/node && idf.py build)"
[ -x "$GOSIM_BIN" ] || fail "gosim binary missing/not executable: $GOSIM_BIN (build it: cd simulator/gosim && go build -o bramble-gosim .)"
[ -f "$SCENARIO" ]  || fail "scenario missing: $SCENARIO"

echo "=== Bramble emulator live smoke test ==="
info "repo:     $REPO_ROOT"
info "node:     $NODE_BIN"
info "scenario: $SCENARIO"
info "log:      $LOG"
echo

# --- launch broker + firmware nodes --------------------------------------
"$GOSIM_BIN" -headless -scenario "$SCENARIO" >"$LOG" 2>&1 &
GOSIM_PID=$!
info "gosim headless pid $GOSIM_PID"

# Count broker attach log lines: `emu-link: node "XXXX" attached as 0x...`.
attach_lines() { grep -E 'emu-link: node "[0-9A-Fa-f]+" attached as' "$LOG" 2>/dev/null; }
attach_ids()   { attach_lines | sed -E 's/.*node "([0-9A-Fa-f]+)".*/\1/'; }

# --- wait for 3 attaches --------------------------------------------------
echo "[1/4] waiting for 3 firmware nodes to attach..."
deadline=$(( $(date +%s) + 40 ))
while :; do
    n=$(attach_ids | sort -u | wc -l)
    [ "$n" -ge 3 ] && break
    if ! kill -0 "$GOSIM_PID" 2>/dev/null; then fail "gosim exited before 3 nodes attached (got $n)"; fi
    [ "$(date +%s)" -ge "$deadline" ] && fail "timeout: only $n distinct nodes attached (want 3)"
    sleep 1
done

mapfile -t IDS < <(attach_ids | sort -u)
green "[1/4] (a) 3 nodes attached, distinct ids: ${IDS[*]}"
[ "${#IDS[@]}" -eq 3 ] || fail "expected exactly 3 distinct node ids, got ${#IDS[@]}"
echo

# --- assert >=1 fb per node ----------------------------------------------
# gosim serializes JSON with alphabetically-sorted keys, so field order is not
# fixed; match "device_fb" and the node id with two independent greps.
fb_count() { grep '"type":"device_fb"' "$LOG" 2>/dev/null | grep -c "\"node\":\"$1\""; }
echo "[2/4] waiting for a framebuffer (boot screen) from each node..."
deadline=$(( $(date +%s) + 30 ))
while :; do
    missing=0
    for id in "${IDS[@]}"; do
        [ "$(fb_count "$id")" -ge 1 ] || missing=1
    done
    [ "$missing" -eq 0 ] && break
    [ "$(date +%s)" -ge "$deadline" ] && fail "timeout: not every node emitted an fb"
    sleep 1
done
for id in "${IDS[@]}"; do
    info "node $id: $(fb_count "$id") fb messages"
done
green "[2/4] (c) every node emitted at least one framebuffer (boot screen)"
echo

# --- report tx/delivery (level b) ----------------------------------------
echo "[3/4] checking transmit pricing/delivery (level b)..."
# The broker emits a deterministic emu_tx per external-node transmission and an
# emu_rx per surviving PHY delivery (extnode.go). Match those exact event types;
# the old pattern had an unterminated "type":"packet literal and matched neither.
TX=$(grep -cE '"type":"emu_tx"|"type":"emu_rx"' "$LOG" 2>/dev/null)
if [ "$TX" -gt 0 ]; then
    green "[3/4] (b) $TX tx/delivery events observed -- full beacon delivery reached"
else
    info "0 tx events: nodes are UNPROVISIONED and INERT (no network key)."
    info "This is the documented maximum for this task (no emu-link provisioning"
    info "message in protocol v1). Acceptance level: attach + hello + fb + persistence."
    green "[3/4] (b) reported: no TX (expected for unprovisioned INERT nodes)"
fi
echo

# --- persistence: kill one node, expect same id to re-attach --------------
echo "[4/4] persistence: killing one node process, expecting SAME id to re-attach..."
before_total=$(attach_ids | wc -l)
victim_pid=$(pgrep -P "$GOSIM_PID" -f "$NODE_BIN" | head -1)
[ -n "$victim_pid" ] || fail "could not find a node process to kill"
info "killing node pid $victim_pid"
kill -9 "$victim_pid" 2>/dev/null || true

deadline=$(( $(date +%s) + 30 ))
while :; do
    now_total=$(attach_ids | wc -l)
    [ "$now_total" -gt "$before_total" ] && break
    if ! kill -0 "$GOSIM_PID" 2>/dev/null; then fail "gosim exited before the node re-attached"; fi
    [ "$(date +%s)" -ge "$deadline" ] && fail "timeout: no re-attach after killing a node"
    sleep 1
done

# The restarted node must re-attach with a PREVIOUSLY SEEN id (identity survived
# in $NODE_DIR/flash.bin): distinct-id count stays 3 and one id now appears >=2x.
distinct_after=$(attach_ids | sort -u | wc -l)
[ "$distinct_after" -eq 3 ] || fail "distinct id count changed to $distinct_after after restart (identity not persistent)"
reattached=$(attach_ids | sort | uniq -c | awk '$1>=2 {print $2}')
[ -n "$reattached" ] || fail "no id re-attached twice: a fresh identity was minted (persistence failed)"
green "[4/4] (d) node $reattached re-attached with its original id (identity persisted)"
echo

green "=== SMOKE PASS ==="
echo "Acceptance level achieved: attach + hello + fb + persistence (a, c, d)."
echo "Level (b) full beacon delivery not reached: firmware boots unprovisioned/INERT."
exit 0
