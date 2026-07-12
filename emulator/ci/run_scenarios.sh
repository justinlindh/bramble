#!/usr/bin/env bash
#
# run_scenarios.sh: the headless emulator scenario suite that gates CI.
#
# Boots the gosim broker headless for each scenario (which spawns the real
# firmware node processes) and asserts on delivered/rendered content. Its exit
# code gates the pipeline: 0 only if every scenario's assertions pass.
#
# Scenarios and what each asserts:
#   emu-channel-delivery  three provisioned pagers; the sender broadcasts a
#                         channel message and BOTH receivers RENDER it on their
#                         e-paper (screen-assert, the highest fidelity level).
#   emu-dm-desync         a DM session establishes and renders on the receiver;
#                         the receiver reboots to create the one-sided-session
#                         desync; the sender's continued DMs make the receiver
#                         log the decrypt failures (the bug symptom) and fire the
#                         #138 self-heal re-handshake. Asserts the ALPHA render
#                         plus both log signatures. (The full heal-and-redeliver
#                         completes but has real-time wall-clock variance, so CI
#                         gates on the deterministic symptom + heal trigger, not
#                         the final post-heal render. See the scenario file.)
#
# PREREQUISITES:
#   1. The linux node binary: emulator/node/build/bramble-node.elf
#      (source $IDF_PATH/export.sh; cd emulator/node; idf.py --preview
#       set-target linux; idf.py build)
#   2. gosim: built here if missing (go build, no IDF needed).
#
# Exit 0 on PASS, non-zero on any failed assertion or setup error.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT" || exit 2

NODE_BIN="emulator/node/build/bramble-node.elf"
GOSIM_DIR="simulator/gosim"
GOSIM_BIN="$GOSIM_DIR/bramble-gosim"
SCEN_DIR="simulator/scenarios"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '  %s\n' "$*"; }

FAILURES=0
CHILD_PIDS=()

# shellcheck disable=SC2317  # reached only via the EXIT/INT/TERM trap
cleanup() {
    for pid in "${CHILD_PIDS[@]:-}"; do
        [ -n "$pid" ] || continue
        if kill -0 "$pid" 2>/dev/null; then
            pkill -P "$pid" 2>/dev/null || true
            kill "$pid" 2>/dev/null || true
        fi
    done
    pkill -f "$NODE_BIN" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail_setup() { red "SETUP FAIL: $*"; exit 2; }

# --- preconditions --------------------------------------------------------
[ -x "$NODE_BIN" ] || fail_setup "node binary missing: $NODE_BIN (build it: cd emulator/node && idf.py build)"
if [ ! -x "$GOSIM_BIN" ]; then
    info "building gosim..."
    ( cd "$GOSIM_DIR" && go build -o bramble-gosim . ) || fail_setup "gosim build failed"
fi

# run_scenario <name> <wall_timeout_s> <logvar>
# Boots gosim headless for the named scenario, writing its event log to a temp
# file whose path is stored in the named variable. Returns the gosim exit code.
run_scenario() {
    local name="$1" budget="$2" __logvar="$3"
    local scen="$SCEN_DIR/$name.json"
    [ -f "$scen" ] || { red "scenario missing: $scen"; return 1; }
    local log; log="$(mktemp -t "emu-$name.XXXXXX.log")"
    printf -v "$__logvar" '%s' "$log"

    info "running $name (budget ${budget}s)..."
    timeout "$budget" "$GOSIM_BIN" -headless -scenario "$scen" >"$log" 2>&1 &
    local pid=$!
    CHILD_PIDS+=("$pid")
    wait "$pid"
    return $?
}

# assert_screen <log> <selector...> : passes selector args straight to
# `bramble-gosim screen-assert`. Records a failure on non-zero exit.
assert_screen() {
    local log="$1"; shift
    if "$GOSIM_BIN" screen-assert -log "$log" "$@"; then
        return 0
    fi
    FAILURES=$((FAILURES + 1))
    return 1
}

# assert_log <log> <needle> <description> : the scenario log must contain needle
# (a firmware console signature). Records a failure otherwise.
assert_log() {
    local log="$1" needle="$2" desc="$3"
    if grep -qF "$needle" "$log"; then
        green "PASS: $desc"
        return 0
    fi
    red "FAIL: $desc (log has no '$needle')"
    FAILURES=$((FAILURES + 1))
    return 1
}

echo "=== Bramble emulator scenario suite ==="
info "repo: $REPO_ROOT"
echo

# --- Scenario 1: channel delivery ----------------------------------------
echo "[1] emu-channel-delivery"
CHAN_LOG=""
run_scenario emu-channel-delivery 70 CHAN_LOG
assert_screen "$CHAN_LOG" -min-nodes 2 -text "HELLO BRAMBLE"
echo

# --- Scenario 2: DM session desync + #138 heal ---------------------------
echo "[2] emu-dm-desync"
DM_LOG=""
run_scenario emu-dm-desync 120 DM_LOG
assert_screen "$DM_LOG" -at "30,0" -text "DM ALPHA"
assert_log "$DM_LOG" "Failed session decrypt" "desync symptom reproduced (stale one-sided DM session)"
assert_log "$DM_LOG" "re-initiating handshake (self-heal)" "#138 receiver-side self-heal engaged"
echo

# --- verdict --------------------------------------------------------------
if [ "$FAILURES" -eq 0 ]; then
    green "=== SCENARIO SUITE PASS ==="
    exit 0
fi
red "=== SCENARIO SUITE FAIL ($FAILURES assertion(s)) ==="
exit 1
