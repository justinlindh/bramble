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
#                         both log signatures. Deterministic assertion, no
#                         retries; a run invalidated by a mid-run node process
#                         death is re-run once (see dm_suite's comment).
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
    # env -u: an exported EMU_SCENARIO_DURATION_MS is channel_suite's knob; it
    # must not leak into other scenarios and silently cap their sim time.
    timeout "$budget" env -u EMU_SCENARIO_DURATION_MS \
        "$GOSIM_BIN" -headless -scenario "$scen" >"$log" 2>&1 &
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

# dump_diagnostics <log> <label> : post-mortem for a failed scenario, printed
# straight into the CI step log. A scenario's gosim event log lives in a mktemp
# file the runner throws away, so without this a pod-only failure is invisible
# and undebuggable from the outside (which is exactly how the render-window
# failures stayed mysterious). Prints the attach/join records, how many
# framebuffer frames each node actually emitted, the screen-assert verdict, and
# the log tail (frames elided; they are base64 blobs).
dump_diagnostics() {
    local log="$1" label="$2"
    echo "--- diagnostics: $label ---"
    echo "[attach/join/death events]"
    grep -E "attached as|node_joined|node_left|supervisor|restart|exited|abort|assert|Segmentation|panic" "$log" | tail -25
    echo "[device_fb frames per node]"
    grep '"type":"device_fb"' "$log" | grep -o '"node":"[^"]*"' | sort | uniq -c | head -10
    echo "[emu_tx per node (did each node key the channel?)]"
    grep '"type":"emu_tx"' "$log" | grep -o '"node":"[^"]*"' | sort | uniq -c | head -10
    echo "[emu_tx airtime distribution (toa_ms: beacons are short, message frames ~2100ms)]"
    grep '"type":"emu_tx"' "$log" | grep -o '"toa_ms":[0-9]*' | sort | uniq -c | head -10
    echo "[emu_rx per node (did the frames reach each node?)]"
    grep '"type":"emu_rx"' "$log" | grep -o '"node":"[^"]*"' | sort | uniq -c | head -10
    echo "[log tail, fb frames and periodic metrics elided]"
    grep -v -e '"type":"device_fb"' -e '"type":"metrics"' "$log" | tail -40
    echo "--- end diagnostics: $label ---"
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
# The firmware nodes run in wall-clock time and the receivers must boot, pass
# their 10 s message-idle threshold, and render the broadcast on the virtual
# e-paper. Neighbor discovery is no longer a variable (a broadcast needs no
# session, and the short emulator beacon interval keeps the mesh warm). Two
# real failure modes were observed on the CPU-limited CI runner pods (cpu 4 /
# memory 5Gi, shared with other jobs), and both are handled here:
#
#   1. The render outruns a fixed window. The old fix was a fixed 36 s window
#      plus a 2x re-roll, which failed BOTH attempts on constrained pods
#      because the window was systematically too tight, not randomly jittery.
#      Now the wait is EVENT-DRIVEN: we widen gosim's real-time cap
#      (EMU_SCENARIO_DURATION_MS) and POLL the growing headless log for the
#      render marker, stopping the instant both receivers have painted. A fast
#      box exits in seconds; a starved pod gets the full budget.
#   2. The whole send burst lands before a receiver's idle threshold. A
#      receiver only auto-opens its Messages screen for a message arriving
#      after its own 10 s message-idle threshold, measured from ITS boot, and
#      pod contention can lag the staggered receiver boots tens of seconds
#      behind the sender's clock. A short burst (formerly 3 sends ending at
#      sender t=20 s) could then land entirely inside the receivers' threshold
#      window, after which nothing would ever render no matter how long the
#      wait. The scenario now sends a long burst (12 sends, 8 s apart, to
#      sender t=100 s) so some sends always postdate every receiver's
#      threshold; the event-driven early exit keeps the long tail free on a
#      fast box.
#
# A genuine delivery regression simply never renders and the wait times out, so
# one generous attempt replaces the old re-roll loop (no retry scaffolding
# remains). Both knobs are overridable for local tuning but default high
# because this script only ever runs as the CI gate.
channel_suite() {
    echo "[1] emu-channel-delivery"
    local scen="$SCEN_DIR/emu-channel-delivery.json"
    [ -f "$scen" ] || { red "scenario missing: $scen"; return 1; }
    local budget_s="${EMU_CHANNEL_BUDGET_S:-180}"
    local sim_ms="${EMU_SCENARIO_DURATION_MS:-150000}"
    local log; log="$(mktemp -t emu-channel.XXXXXX.log)"

    info "running emu-channel-delivery (render budget ${budget_s}s, sim cap $((sim_ms / 1000))s)..."
    EMU_SCENARIO_DURATION_MS="$sim_ms" \
        timeout "$budget_s" "$GOSIM_BIN" -headless -scenario "$scen" >"$log" 2>&1 &
    local pid=$!
    CHILD_PIDS+=("$pid")

    local rendered=1 deadline
    deadline=$(( $(date +%s) + budget_s ))
    while :; do
        if "$GOSIM_BIN" screen-assert -log "$log" -min-nodes 2 -text "HELLO BRAMBLE" \
            >/dev/null 2>&1; then
            rendered=0
            break
        fi
        # gosim exited (hit its sim cap or the outer timeout) without a render, or
        # the wall-clock budget elapsed: stop polling and do a final check below.
        kill -0 "$pid" 2>/dev/null || break
        [ "$(date +%s)" -lt "$deadline" ] || break
        # 2s between scans: each scan decodes every frame in the growing log,
        # and on a CPU-capped pod a 1s cadence would take non-trivial CPU away
        # from the very firmware nodes it is waiting on.
        sleep 2
    done

    # Stop the run; it has either rendered or run out of budget.
    kill "$pid" 2>/dev/null || true
    pkill -P "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true

    # Final check in case the render landed in the last frames flushed as gosim exited.
    if [ "$rendered" -ne 0 ] && "$GOSIM_BIN" screen-assert -log "$log" -min-nodes 2 \
        -text "HELLO BRAMBLE" >/dev/null 2>&1; then
        rendered=0
    fi

    if [ "$rendered" -eq 0 ]; then
        green "PASS: emu-channel-delivery: rendered on both receivers"
        return 0
    fi
    red "FAIL: emu-channel-delivery: not rendered on >= 2 nodes within ${budget_s}s"
    # Visible verdict ("rendered on N node(s), want >= 2") plus the post-mortem.
    "$GOSIM_BIN" screen-assert -log "$log" -min-nodes 2 -text "HELLO BRAMBLE" || true
    dump_diagnostics "$log" "emu-channel-delivery"
    return 1
}

# --- Scenario 2: DM session desync + #138 heal ---------------------------
# Deterministic construction, so the ASSERTION gets no retries. The receiver
# drops its DM session half in-process at a fixed time
# (EMU_DROP_DM_SESSION_AT_MS) while still neighboring the sender, and the
# sender's DM burst is scheduled to begin AFTER that drop, so the sender's
# stale-session DM is GUARANTEED to land on a receiver with no session on the
# very first post-drop send. That removes the reboot-era races (RAM-clear
# timing, beacon re-acquisition, and the stale DM having to hit a narrow
# post-reboot window) that made this scenario probabilistic and forced the old
# re-roll loop. A genuine regression (the symptom or the #138 self-heal never
# fires) fails a VALID run immediately. The 120s budget is only a hang guard;
# the scenario itself finishes in ~80s of wall clock.
#
# INVALID-RUN rerun (not an assertion retry): the construction has one hard
# precondition, that both node processes stay up for the whole run. A node
# process that dies mid-run is restarted by gosim's supervisor with cleared RAM
# (observed on a pod: the sender died ~17s in, and its restarted self held no
# stale session, so the desync could not exist and the assertion had nothing to
# find). That is infrastructure destroying the constructed state, not the bug
# failing to reproduce, so such a run proves nothing either way. It is detected
# EXACTLY (more node_joined events than the scenario's two nodes) and the run
# is repeated once, with the death evidence dumped loudly (the supervisor logs
# every unexpected exit with its code/signal). Two invalidated runs in a row,
# or an assertion failure on any valid run, fail the suite. The mid-run death
# itself is a real reliability bug being tracked separately; this rerun keeps
# the gate honest about what THIS scenario asserts without masking that bug
# (the evidence prints on every occurrence).
dm_suite() {
    echo "[2] emu-dm-desync"
    local attempt DM_LOG joins
    for attempt in 1 2; do
        DM_LOG=""
        run_scenario emu-dm-desync 120 DM_LOG
        if "$GOSIM_BIN" screen-assert -log "$DM_LOG" -at "30,0" -text "DM ALPHA" >/dev/null 2>&1 \
           && grep -qF "Failed session decrypt" "$DM_LOG" \
           && grep -qF "re-initiating handshake (self-heal)" "$DM_LOG"; then
            green "PASS: emu-dm-desync: ALPHA render + desync symptom + #138 self-heal"
            return 0
        fi
        joins="$(grep -c '"type":"node_joined"' "$DM_LOG")"
        if [ "$joins" -gt 2 ] && [ "$attempt" -eq 1 ]; then
            red "INVALID RUN: a node process died and was restarted mid-scenario ($joins joins for 2 nodes), destroying the constructed desync state; re-running once"
            dump_diagnostics "$DM_LOG" "emu-dm-desync (invalidated run)"
            continue
        fi
        red "FAIL: emu-dm-desync did not reproduce the desync symptom + #138 self-heal"
        "$GOSIM_BIN" screen-assert -log "$DM_LOG" -at "30,0" -text "DM ALPHA" || true
        dump_diagnostics "$DM_LOG" "emu-dm-desync"
        return 1
    done
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
