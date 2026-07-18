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
#                         the receiver then DROPS its session half in-process at a
#                         fixed instant (EMU_DROP_DM_SESSION_AT_MS) to construct
#                         the one-sided-session desync deterministically, while
#                         still neighboring the sender; the sender's continued DMs
#                         land on a receiver with no session, so it logs the
#                         decrypt failures (the bug symptom) and fires the #138
#                         self-heal re-handshake. Asserts the ALPHA render plus
#                         both log signatures. Deterministic: runs once, no retry.
#                         (The full heal-and-redeliver completes but has real-time
#                         wall-clock variance, so CI gates on the symptom + heal
#                         trigger, not the final post-heal render. See the
#                         scenario file.)
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
    # Belt and suspenders: a name-wide sweep catches any gosim left behind by an
    # aborted run (gosim's own SIGTERM handler then reaps its nodes), so strays
    # can never linger and contaminate a later run's real-time timing.
    pkill -f "$GOSIM_BIN" 2>/dev/null || true
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

# The two scenarios run SEQUENTIALLY, not in parallel. Each spawns real firmware
# node processes that run in wall-clock time, and running both suites at once put
# 5 nodes on the host at the same time; on a loaded runner that CPU contention
# starved the real-time firmware (missed render windows, and worse, missed the
# single beacon exchange that neighbor discovery depended on), which is a flake
# source in its own right. The scenarios are isolated (gosim keys its emu-link
# socket and each node's NODE_DIR to its own PID, and headless never binds a TCP
# port), so they COULD run concurrently, but determinism on a shared CI runner is
# worth more than the few tens of seconds parallelism saved. Each suite function
# returns 0 on PASS and 1 on FAIL; we run them one after the other and fold in
# each result.

# --- Scenario 1: channel delivery ----------------------------------------
# The firmware nodes run in wall-clock time inside a 36 s scenario budget, and
# the receivers must boot, pass their 10 s message-idle threshold, and render
# the broadcast on the virtual e-paper within it. Neighbor discovery is no
# longer a variable (a broadcast needs no session, and the short emulator beacon
# interval keeps the mesh warm), but the final e-paper RENDER still has some
# wall-clock jitter under load: the inbound message has to auto-open the Messages
# screen and paint before the budget ends. That residual timing sensitivity is
# not something this delivery test can construct away, so a single low retry
# budget stays as a jitter absorber. A genuine delivery regression still fails
# both attempts, so the retry masks jitter, not a real break.
channel_suite() {
    echo "[1] emu-channel-delivery"
    local CHAN_ATTEMPTS=2
    local attempt CHAN_LOG
    for attempt in $(seq 1 "$CHAN_ATTEMPTS"); do
        CHAN_LOG=""
        run_scenario emu-channel-delivery 70 CHAN_LOG
        if "$GOSIM_BIN" screen-assert -log "$CHAN_LOG" -min-nodes 2 -text "HELLO BRAMBLE" \
            >/dev/null 2>&1; then
            green "PASS: emu-channel-delivery (attempt $attempt/$CHAN_ATTEMPTS): rendered on both receivers"
            return 0
        fi
        info "attempt $attempt/$CHAN_ATTEMPTS missed the render window; re-rolling..."
    done
    red "FAIL: emu-channel-delivery: not rendered on >= 2 nodes in any of $CHAN_ATTEMPTS attempts"
    return 1
}

# --- Scenario 2: DM session desync + #138 heal ---------------------------
# Deterministic, so it runs ONCE with no retry loop. The receiver drops its DM
# session half in-process at a fixed time (EMU_DROP_DM_SESSION_AT_MS) while still
# neighboring the sender, and the sender's DM burst is scheduled to begin AFTER
# that drop, so the sender's stale-session DM is GUARANTEED to land on a receiver
# with no session on the very first post-drop send. That removes the reboot-era
# races (RAM-clear timing, beacon re-acquisition, and the stale DM having to hit
# a narrow post-reboot window) that made this scenario probabilistic and forced
# the old re-roll loop. A genuine regression (the symptom or the #138 self-heal
# never fires) fails the single run, exactly as it should. The 120s budget is
# only a hang guard; the scenario itself finishes in ~80s of wall clock.
dm_suite() {
    echo "[2] emu-dm-desync"
    local DM_LOG=""
    run_scenario emu-dm-desync 120 DM_LOG
    if "$GOSIM_BIN" screen-assert -log "$DM_LOG" -at "30,0" -text "DM ALPHA" >/dev/null 2>&1 \
       && grep -qF "Failed session decrypt" "$DM_LOG" \
       && grep -qF "re-initiating handshake (self-heal)" "$DM_LOG"; then
        green "PASS: emu-dm-desync: ALPHA render + desync symptom + #138 self-heal"
        return 0
    fi
    red "FAIL: emu-dm-desync did not reproduce the desync symptom + #138 self-heal"
    return 1
}

# Run the two suites one after the other so only one scenario's firmware nodes
# are on the host at a time (see the isolation note above).
channel_suite; chan_rc=$?
echo
dm_suite; dm_rc=$?
echo

[ "$chan_rc" -eq 0 ] || FAILURES=$((FAILURES + 1))
[ "$dm_rc" -eq 0 ] || FAILURES=$((FAILURES + 1))

# --- verdict --------------------------------------------------------------
if [ "$FAILURES" -eq 0 ]; then
    green "=== SCENARIO SUITE PASS ==="
    exit 0
fi
red "=== SCENARIO SUITE FAIL ($FAILURES assertion(s)) ==="
exit 1
