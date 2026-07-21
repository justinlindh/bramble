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
#   emu-gps-fix           one GPS-capable pager; the broker feeds it NMEA
#                         sentences synthesized from its slot position while
#                         its GPS gate is on, and the firmware must acquire a
#                         fix. Exercises the gps_virt reader-to-pump-task
#                         crossing (asserts the gpsgate telemetry plus the
#                         firmware's 'GPS position updated' console line at the
#                         origin latitude). One run, no retries; any node death
#                         fails the suite.
#   emu-dm-desync         a DM session establishes and renders on the receiver;
#                         the receiver then DROPS its session half in-process at a
#                         fixed instant (EMU_DROP_DM_SESSION_AT_MS) to construct
#                         the one-sided-session desync deterministically, while
#                         still neighboring the sender; the sender's continued DMs
#                         land on a receiver with no session, so it logs the
#                         decrypt failures (the bug symptom) and fires the #138
#                         self-heal re-handshake. Asserts the ALPHA render plus
#                         both log signatures. One run, no retries; any node
#                         death fails the suite (see check_no_deaths).
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

# Scenario logs persist here instead of dying with a mktemp: on CI the
# workflow uploads this directory as an artifact when the suite fails, so
# death/crash evidence survives the runner pod. Locally it defaults to a
# temp dir the developer can inspect after a failure.
LOG_DIR="${EMU_CI_LOG_DIR:-$(mktemp -d -t emu-scenario-logs.XXXXXX)}"
mkdir -p "$LOG_DIR"

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
# Always build, never build-if-missing: `go build` is itself the staleness
# check and a near no-op when nothing changed (CI's earlier Build-gosim step
# keeps it a cache hit there). The old existence check once let a
# checked-out gosim binary from an older commit run as the broker, and a
# scenario then failed for reasons that looked like a firmware regression
# (the pre-NMEA broker never fed the GPS fix scenario).
info "building gosim..."
( cd "$GOSIM_DIR" && go build -o bramble-gosim . ) || fail_setup "gosim build failed"

# run_scenario <name> <wall_timeout_s> <logvar>
# Boots gosim headless for the named scenario, writing its event log to a temp
# file whose path is stored in the named variable. Returns the gosim exit code.
run_scenario() {
    local name="$1" budget="$2" __logvar="$3"
    local scen="$SCEN_DIR/$name.json"
    [ -f "$scen" ] || { red "scenario missing: $scen"; return 1; }
    local log; log="$LOG_DIR/emu-$name-$(date +%s).log"
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
    grep -iE "attached as|node_joined|node_left|supervisor|restart|exited|abort|assert|segmentation|panic|reboot" "$log" | tail -25
    # For each unexpected node death, print the console lines that led up to
    # it (frames/metrics/packet noise elided): the death REASON is in the
    # node's last words, and the log tail alone usually post-dates them.
    grep -n "exited unexpectedly" "$log" | cut -d: -f1 | while read -r ln; do
        echo "[pre-death context before log line $ln]"
        start=$((ln > 400 ? ln - 400 : 1))
        sed -n "${start},${ln}p" "$log" \
            | grep -v -e '"type":"device_fb"' -e '"type":"metrics"' \
                      -e '"type":"packet_sent"' -e '"type":"emu_rx"' \
                      -e '"type":"emu_tx"' -e '"type":"device_ind"' \
            | tail -15
    done
    echo "[device_fb frames per node]"
    grep '"type":"device_fb"' "$log" | grep -o '"node":"[^"]*"' | sort | uniq -c | head -10
    echo "[emu_tx per node (did each node key the channel?)]"
    grep '"type":"emu_tx"' "$log" | grep -o '"node":"[^"]*"' | sort | uniq -c | head -10
    echo "[emu_tx airtime distribution (toa_ms: beacons are short, message frames ~2100ms)]"
    grep '"type":"emu_tx"' "$log" | grep -o '"toa_ms":[0-9]*' | sort | uniq -c | head -10
    echo "[emu_rx per node (did the frames reach each node?)]"
    grep '"type":"emu_rx"' "$log" | grep -o '"node":"[^"]*"' | sort | uniq -c | head -10
    echo "[dm construction markers (drop/purge/symptom/heal), if any]"
    grep -E "dropped DM session|reboot-faithful drop|EMU_DROP_DM_SESSION_AT_MS reached|Failed session decrypt|re-initiating handshake" "$log" | head -15
    echo "[log tail, fb frames and periodic metrics elided]"
    grep -v -e '"type":"device_fb"' -e '"type":"metrics"' "$log" | tail -40
    echo "--- end diagnostics: $label ---"
}

# check_no_deaths <log> <expected_joins> <label> : STRICT death rule, applied
# to EVERY run of every scenario, pass or fail. Both POSIX-port death causes
# (FreeRTOS entered from the reader thread; tasks futex-blocked while holding
# FreeRTOS mutexes) are fixed on this branch, so a mid-scenario node death no
# longer has a known benign cause: any death is treated as a suite FAILURE
# with a full post-mortem, never absorbed by a retry or a lucky assertion
# (a crash-and-restart can still render late sends and green the assertion;
# adversarial review showed a 30% crash regression would pass a rerun-based
# gate ~91% of the time). Detected two ways: the supervisor's unexpected-exit
# log line, and more node_joined events than the scenario has nodes (a
# restart re-attaches and re-joins).
check_no_deaths() {
    local log="$1" expected="$2" label="$3"
    local deaths joins
    deaths="$(grep -c "exited unexpectedly" "$log")" || true
    joins="$(grep -c '"type":"node_joined"' "$log")" || true
    if [ "${deaths:-0}" -eq 0 ] && [ "${joins:-0}" -le "$expected" ]; then
        return 0
    fi
    red "FAIL: $label: node process death mid-scenario ($deaths unexpected exit(s), $joins joins for $expected nodes); deaths are hard failures, see the post-mortem"
    dump_diagnostics "$log" "$label (node death)"
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

    # STRICT death rule: even a run that rendered fine fails if a node died
    # mid-scenario (a restarted receiver can still catch late sends and green
    # the assertion, hiding a crash regression). See check_no_deaths.
    check_no_deaths "$log" 3 "emu-channel-delivery" || return 1

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
# Deterministic construction, ONE run, no retries of any kind. The receiver
# drops its DM session half in-process at a fixed time
# (EMU_DROP_DM_SESSION_AT_MS) while still neighboring the sender, and the
# sender's DM burst is scheduled to begin AFTER that drop, so the sender's
# stale-session DM is GUARANTEED to land on a receiver with no session on the
# very first post-drop send. The drop is reboot-faithful (it also purges the
# receiver's pending-ack retransmits and awaiting-session queue for the peer,
# exactly what the reboot it emulates would destroy), which removes the last
# known construction race. A genuine regression fails the run, exactly as it
# should: it never produces the markers and the wait times out. The wait is
# event-driven with a wide budget (see the comment inside dm_suite); a
# healthy box exits at ~90s, only a starved one uses the headroom.
#
# There is deliberately NO invalidation-rerun here anymore: both POSIX-port
# node-death causes are fixed, so a mid-run death has no known benign cause
# and is a hard suite failure via check_no_deaths (a rerun would absorb
# intermittent crash regressions; see the check_no_deaths comment). If a new
# port bug appears, this gate fails loudly with a post-mortem, which is the
# correct behavior for a required gate.
dm_suite() {
    echo "[2] emu-dm-desync"
    # EVENT-DRIVEN, exactly like channel_suite: poll the growing log for the
    # three gate markers and stop the instant all are present, under a WIDE
    # wall budget. The old shape (fixed 150s sim cap, assert after exit) was
    # wall-clock arithmetic applied to a subjective-time workload: on a
    # starved runner pod the firmware's subjective clock falls behind wall
    # time (a cluster-wide IO storm once slowed it to ~1/4 speed), the cap
    # truncated the scenario at ~45 subjective seconds, TX windows smeared
    # into ~45% half-duplex frame loss, and the KE handshake never got the
    # time it needed. Polling makes a healthy box exit at ~90s while a
    # starved one keeps its full subjective schedule; a genuine regression
    # still simply never produces the markers and times out.
    local budget_s="${EMU_DM_BUDGET_S:-420}"
    local sim_ms="${EMU_DM_SIM_CAP_MS:-420000}"
    local scen="$SCEN_DIR/emu-dm-desync.json"
    [ -f "$scen" ] || { red "scenario missing: $scen"; return 1; }
    local DM_LOG; DM_LOG="$LOG_DIR/emu-emu-dm-desync-$(date +%s).log"

    info "running emu-dm-desync (marker budget ${budget_s}s, sim cap $((sim_ms / 1000))s)..."
    EMU_SCENARIO_DURATION_MS="$sim_ms" \
        timeout "$budget_s" "$GOSIM_BIN" -headless -scenario "$scen" >"$DM_LOG" 2>&1 &
    local pid=$!
    CHILD_PIDS+=("$pid")

    dm_markers_present() {
        grep -qF "Failed session decrypt" "$DM_LOG" \
            && grep -qF "re-initiating handshake (self-heal)" "$DM_LOG" \
            && "$GOSIM_BIN" screen-assert -log "$DM_LOG" -at "30,0" -text "DM ALPHA" \
                >/dev/null 2>&1
    }

    local seen=1 deadline
    deadline=$(( $(date +%s) + budget_s ))
    while :; do
        if dm_markers_present; then
            seen=0
            break
        fi
        kill -0 "$pid" 2>/dev/null || break
        [ "$(date +%s)" -lt "$deadline" ] || break
        # 3s cadence: the screen-assert decodes every frame in the growing
        # log, and a tighter loop would steal CPU from the very nodes it
        # waits on (same reasoning as channel_suite's 2s).
        sleep 3
    done

    kill "$pid" 2>/dev/null || true
    pkill -P "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true

    # Final check: the last markers may have landed in the frames gosim
    # flushed on exit.
    if [ "$seen" -ne 0 ] && dm_markers_present; then
        seen=0
    fi

    # STRICT death rule: deaths fail the run regardless of assertions.
    check_no_deaths "$DM_LOG" 2 "emu-dm-desync" || return 1

    if [ "$seen" -eq 0 ]; then
        green "PASS: emu-dm-desync: ALPHA render + desync symptom + #138 self-heal"
        return 0
    fi
    red "FAIL: emu-dm-desync did not reproduce the desync symptom + #138 self-heal"
    "$GOSIM_BIN" screen-assert -log "$DM_LOG" -at "30,0" -text "DM ALPHA" || true
    dump_diagnostics "$DM_LOG" "emu-dm-desync"
    return 1
}

# --- Scenario 3: GPS fix ---------------------------------------------------
# One GPS-capable pager. The firmware runs gps_init at boot and opens its GPS
# power gate; the broker answers the gpsgate-on message by feeding synthesized
# NMEA sentences from the node's slot position on the sim clock (nmea.go). The
# sentences arrive on the emu-link reader thread and cross into gps_virt's
# sentence ring, where the FreeRTOS pump task parses them and fires the fix
# callback: this is the exact reader-to-task crossing the deferred-ring fix
# exists for, so a regression either kills the node (caught by the strict
# no-deaths rule) or never logs a fix. Deterministic, one run, no retries. The
# fix latitude is the canonical NMEA origin (48.117), a fixed placeholder
# coordinate; asserting the exact value proves the whole gate/feed/parse/fix
# path rather than merely that some log line appeared.
gps_suite() {
    echo "[3] emu-gps-fix"
    local GPS_LOG=""
    # Budget is a hang-guard, not a pacing knob: gosim exits on its own at the
    # scenario's 90s duration_ms, so on a healthy box this returns in ~90s. The
    # budget must exceed the WALL-CLOCK time a CPU-starved runner takes to reach
    # 90s of sim, or timeout SIGTERMs the node mid-run and check_no_deaths reads
    # that as a false death. 90s (== duration, zero margin) did exactly that on a
    # slow pod; 180s matches the other suites' 2x margin.
    run_scenario emu-gps-fix 180 GPS_LOG

    # STRICT death rule first: a death is the primary regression signal here.
    check_no_deaths "$GPS_LOG" 1 "emu-gps-fix" || return 1

    if grep -qF '"type":"device_gpsgate"' "$GPS_LOG" \
       && grep -qF "GPS position updated: lat=48.117" "$GPS_LOG"; then
        green "PASS: emu-gps-fix: GPS gate opened + fix acquired at origin"
        return 0
    fi
    red "FAIL: emu-gps-fix did not open the GPS gate and acquire a fix"
    dump_diagnostics "$GPS_LOG" "emu-gps-fix"
    return 1
}

# Run the suites one after the other so only one scenario's firmware nodes
# are on the host at a time (see the isolation note above).
channel_suite; chan_rc=$?
echo
dm_suite; dm_rc=$?
echo
gps_suite; gps_rc=$?
echo

[ "$chan_rc" -eq 0 ] || FAILURES=$((FAILURES + 1))
[ "$dm_rc" -eq 0 ] || FAILURES=$((FAILURES + 1))
[ "$gps_rc" -eq 0 ] || FAILURES=$((FAILURES + 1))

# --- verdict --------------------------------------------------------------
if [ "$FAILURES" -eq 0 ]; then
    green "=== SCENARIO SUITE PASS ==="
    exit 0
fi
red "=== SCENARIO SUITE FAIL ($FAILURES assertion(s)) ==="
exit 1
