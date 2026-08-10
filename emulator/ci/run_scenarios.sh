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
#   emu-parked-delivery   park-b holds its own mesh_task_start (no radio at
#                         all) for a fixed window, so it is genuinely
#                         unreachable; park-a targets park-b's real address via
#                         a handoff file (never a beacon, so the eventual
#                         rejoin is a genuine is_new_peer edge, not a table
#                         update) and sends two DMs that fail and get parked.
#                         When park-b's window ends and it joins the mesh,
#                         park-a's rejoin-triggered flush redelivers both.
#                         Asserts both distinct message bodies land in park-b's
#                         console log, individually identified, not a count.
#                         One run, no retries; any node death fails the suite.
#                         See the scenario file for the full construction.
#   emu-parked-delivery-live  the companion case: b2-b never leaves the mesh,
#                         so the rejoin edge can never fire again for it.
#                         b2-a and b2-b establish a normal DM session, b2-b
#                         drops its own session half (EMU_DROP_DM_SESSION_AT_MS)
#                         while staying fully up and beaconing on schedule
#                         (an ACK-loss condition, not an outage), and b2-a's
#                         next DM fails via its own ACTIVE-session pending-ack
#                         exhaustion and gets parked. Only the beacon-armed
#                         retry (main/parked_retry.c), not the rejoin edge, can
#                         ever redeliver it. Asserts the specific message body
#                         lands in b2-b's console log. One run, no retries;
#                         any node death fails the suite.
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
        kill -0 "$pid" 2>/dev/null || continue
        # Reap by PROCESS GROUP, never by process name. Every scenario launches
        # gosim under `timeout`, which puts itself in a new process group before
        # forking, so the recorded pid leads a group containing gosim and every
        # firmware node it spawned. One negative-pid kill reaps that whole tree,
        # including nodes whose parent is already gone, and gosim's own SIGTERM
        # handler (runRealtimeHeadless) reaps its nodes on the way out, so no
        # stray can outlive this run and contaminate a later one's timing.
        #
        # A name pattern cannot do this safely, because it cannot tell THIS
        # run's processes from another run's: the node's argv is the same
        # relative binary path in every checkout. With two suites overlapping on
        # one host (two shells, two agent sessions, two worktrees), the first to
        # exit SIGTERMed the second's firmware nodes mid-scenario, the victim's
        # supervisor logged "exited unexpectedly: signal: terminated",
        # check_no_deaths read that as a node death exactly as it should, and a
        # healthy run went red. Observed 2026-08-08 on two suites started 6s
        # apart. Same rule, and the same reasoning, as emulator/e2e/run_e2e.sh's
        # trap: kill the pids this run owns, nothing else.
        kill -TERM "-$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
    done
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

# check_no_deaths <log> <expected_joins> <label> [prefix_lines] : STRICT death
# rule, applied to EVERY run of every scenario, pass or fail. Both POSIX-port
# death causes (FreeRTOS entered from the reader thread; tasks futex-blocked
# while holding FreeRTOS mutexes) are fixed on this branch, so a mid-scenario
# node death no longer has a known benign cause: any death is treated as a
# suite FAILURE with a full post-mortem, never absorbed by a retry or a lucky
# assertion (a crash-and-restart can still render late sends and green the
# assertion; adversarial review showed a 30% crash regression would pass a
# rerun-based gate ~91% of the time). Detected two ways: the supervisor's
# unexpected-exit log line, and more node_joined events than the scenario has
# nodes (a restart re-attaches and re-joins).
#
# prefix_lines SCOPES THE RULE TO THE SCENARIO, NOT THE SUITE'S OWN TEARDOWN.
# The marker-driven suites stop a PASSING run by killing gosim and its
# children; the supervisor can observe its node child dying from that same
# kill and log "exited unexpectedly: signal: terminated" in the instant
# before it dies itself. Whether that line lands in the log is a scheduling
# race, and on 2026-07-24 it landed: a healthy run (all markers present at
# ~30s) was failed by its own teardown artifact. Callers snapshot the log's
# line count AT THE MOMENT their assertions passed, before any kill, and this
# check reads only that prefix. A real death always precedes the assertions
# passing, so it is always inside the prefix; only post-teardown fallout is
# excluded. Callers on their failure paths snapshot after the wait loop ends,
# which is still before any kill, so the strict rule is untouched there.
check_no_deaths() {
    local log="$1" expected="$2" label="$3" prefix_lines="${4:-}"
    local deaths joins scope
    scope="$log"
    if [ -n "$prefix_lines" ]; then
        scope="$(mktemp -t emu-death-scope.XXXXXX)"
        head -n "$prefix_lines" "$log" > "$scope"
    fi
    deaths="$(grep -c "exited unexpectedly" "$scope")" || true
    joins="$(grep -c '"type":"node_joined"' "$scope")" || true
    if [ -n "$prefix_lines" ]; then
        rm -f "$scope"
    fi
    if [ "${deaths:-0}" -eq 0 ] && [ "${joins:-0}" -le "$expected" ]; then
        return 0
    fi
    red "FAIL: $label: node process death mid-scenario ($deaths unexpected exit(s), $joins joins for $expected nodes); deaths are hard failures, see the post-mortem"
    dump_diagnostics "$log" "$label (node death)"
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

    # Snapshot the scenario window BEFORE any teardown signal: the death rule
    # below judges only lines logged up to this point, so the suite's own
    # kill cannot manufacture a "death" (see check_no_deaths).
    local scenario_lines
    scenario_lines="$(wc -l < "$log")"

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
    check_no_deaths "$log" 3 "emu-channel-delivery" "$scenario_lines" || return 1

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
    #
    # SIZE THE BUDGET AS A STARVATION MULTIPLE, NOT A DURATION. Both knobs
    # are wall clock (gosim runs the firmware in real time), and the markers
    # land by ~90 subjective seconds, so budget/90 is the starvation factor
    # this gate survives. The previous 420s pair gave ~4.7x over that
    # schedule, thinner than the ~12x channel_suite has run at since its
    # redesign (180s over a ~15s render schedule) with zero false fails;
    # 1200s puts this gate in the same margin class, since the historical
    # IO-storm starvation (~4x observed) left no headroom at 4.7x. Cost on a
    # healthy box: none (it still exits at ~90s); cost on a genuine
    # regression: red after 20 min instead of 7. The outer budget runs 30s
    # past the cap so a capped run self-exits cleanly and flushes its final
    # frames before timeout SIGTERMs it, mirroring channel_suite's 180/150
    # gap. Note the 2026-07-23 gate failures were NOT this budget: they were
    # the phase-2 queue-eviction race fixed in emulator/node/emu_autosend.c
    # (ALPHA evicted from the awaiting-session queue before the KE handshake
    # completed, making the render impossible in any budget).
    local budget_s="${EMU_DM_BUDGET_S:-1230}"
    local sim_ms="${EMU_DM_SIM_CAP_MS:-1200000}"
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

    # Snapshot the scenario window BEFORE any teardown signal: the death rule
    # below judges only lines logged up to this point, so the suite's own
    # kill cannot manufacture a "death" (see check_no_deaths; this exact race
    # failed a healthy run on 2026-07-24).
    local scenario_lines
    scenario_lines="$(wc -l < "$DM_LOG")"

    kill "$pid" 2>/dev/null || true
    pkill -P "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true

    # Final check: the last markers may have landed in the frames gosim
    # flushed on exit.
    if [ "$seen" -ne 0 ] && dm_markers_present; then
        seen=0
    fi

    # STRICT death rule: deaths fail the run regardless of assertions.
    check_no_deaths "$DM_LOG" 2 "emu-dm-desync" "$scenario_lines" || return 1

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

# location_channel_verdict <log> : succeeds when one node broadcast a channel
# location share and a DIFFERENT node decoded it and attributed it to the
# sharer. Console records are one JSON object per line carrying the emitting
# node's address, so the sharer's address is read off its TX line and then
# required to appear both as the source in the receiver's RX line and as a
# different emitting node. Comparing the two node ids is what makes this a
# delivery assertion rather than a log-line assertion: a node that decoded its
# own frame, or a single node logging both, does not pass.
LOCATION_SHARER=""
LOCATION_RECEIVER=""
location_channel_verdict() {
    local log="$1"
    LOCATION_SHARER=""
    LOCATION_RECEIVER=""
    local tx_line
    tx_line="$(grep -F 'TX location (channel)' "$log" | head -1)"
    [ -n "$tx_line" ] || return 1
    LOCATION_SHARER="$(printf '%s' "$tx_line" | sed -n 's/.*"node":"\([0-9A-F]*\)".*/\1/p')"
    [ -n "$LOCATION_SHARER" ] || return 1
    LOCATION_RECEIVER="$(grep -F "RX location from $LOCATION_SHARER" "$log" \
        | sed -n 's/.*"node":"\([0-9A-F]*\)".*/\1/p' | head -1)"
    [ -n "$LOCATION_RECEIVER" ] || return 1
    [ "$LOCATION_RECEIVER" != "$LOCATION_SHARER" ] || return 1

    # The seeded public-channel rule must be gone, not merely unsent. A rule the
    # send path refuses and the config surface will not delete would otherwise
    # sit in NVS forever, which is the wedged state this asserts against.
    grep -qF 'removed location rule lch_00' "$log"
}

# Two pagers, one channel location target, no DM between them. Both nodes derive
# the same keyed channel from EMU_LOCATION_CHANNEL_PSK, and only the sharer sets
# EMU_LOCATION_SHARE, so its location namespace is seeded with the same keys and
# rule format bramble.setLocationConfig writes. The mesh task's real policy tick
# has to collect the channel target, encrypt under the channel key and broadcast
# it, and the listener has to authenticate, decrypt and cache it. The channel is
# keyed rather than the public one because the public channel cannot carry
# location: its PSK is well known. Neither node has a route or a DM session to
# the other, which is the condition a channel share exists to work under. This
# catches the failure a parser unit test cannot see: configuration accepted,
# nothing transmitted.
location_suite() {
    echo "[4] emu-location-channel"
    local scen="$SCEN_DIR/emu-location-channel.json"
    [ -f "$scen" ] || { red "scenario missing: $scen"; return 1; }
    local budget_s="${EMU_LOCATION_BUDGET_S:-240}"
    local log; log="$LOG_DIR/emu-location-channel-$(date +%s).log"

    info "running emu-location-channel (share budget ${budget_s}s)..."
    env -u EMU_SCENARIO_DURATION_MS \
        timeout "$budget_s" "$GOSIM_BIN" -headless -scenario "$scen" >"$log" 2>&1 &
    local pid=$!
    CHILD_PIDS+=("$pid")

    # Poll rather than wait out duration_ms: the first share lands about 30s in
    # on a healthy box, and stopping there keeps the common case cheap while
    # leaving the full budget for a starved runner.
    local shared=1 deadline
    deadline=$(( $(date +%s) + budget_s ))
    while :; do
        if location_channel_verdict "$log"; then
            shared=0
            break
        fi
        kill -0 "$pid" 2>/dev/null || break
        [ "$(date +%s)" -lt "$deadline" ] || break
        sleep 2
    done

    # Snapshot the scenario window before any teardown signal, so this suite's
    # own kill cannot be read as a node death (see check_no_deaths).
    local scenario_lines
    scenario_lines="$(wc -l < "$log")"

    kill "$pid" 2>/dev/null || true
    pkill -P "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true

    # Final check in case the share landed in the last lines flushed on exit.
    if [ "$shared" -ne 0 ] && location_channel_verdict "$log"; then
        shared=0
    fi

    check_no_deaths "$log" 2 "emu-location-channel" "$scenario_lines" || return 1

    if [ "$shared" -eq 0 ]; then
        green "PASS: emu-location-channel: $LOCATION_SHARER shared to the channel, $LOCATION_RECEIVER decoded it"
        return 0
    fi
    red "FAIL: emu-location-channel: no node received a channel location share"
    dump_diagnostics "$log" "emu-location-channel"
    return 1
}

# --- Scenario 5: parked delivery -------------------------------------------
# park-b holds its own mesh_task_start (main.c's EMU_MESH_START_DELAY_MS,
# 180s) for the whole failure phase, so it transmits and receives nothing at
# all: a genuinely faithful outage, not a beacon-suppression trick. park-a
# resolves park-b's real, randomly-generated address from a handoff file
# (EMU_AUTO_SEND_TO=file:peer_addr.txt, written by park-b at boot well before
# its mesh delay elapses; see emu_autosend.c's resolve_dest_from_file) rather
# than "neighbor" resolution, because "neighbor" blocks on an actual beacon by
# design and park-a must never have heard one before the rejoin:
# mesh_flush_parked_for only fires on an is_new_peer edge (mesh_beacon.c),
# which is a first-ever admission to the neighbor table, not a refresh of an
# existing one. With park-b genuinely off the mesh, park-a's two scripted DMs
# ('PARK ALPHA', 'PARK BETA', a few seconds apart) can find no route, and each
# queued-for-route entry expires MSG_STATUS_FAILED after the flat 60s route
# TTL, checked on mesh_task's 60s purge cadence (so up to ~120s worst case if
# a send lands right after a tick fires; see emu_autosend.c's park_test_task
# for the full derivation). park-a's EMU_AUTO_PARK=1 parks each individually
# via mesh_park_message once FAILED, the only lever available here: the
# T-Deck's LVGL Queue button (scr_chat_messages.c) is not compiled for the
# virtual-pager board this emulator uses, so this scenario proves the
# mesh-level path (park, rejoin, redeliver), not the button that reaches it on
# real hardware.
#
# The wait is EVENT-DRIVEN like the other suites: poll the growing log for
# the two individually-identified '[MSG from <addr>] PARK ALPHA' / '... PARK
# BETA' lines (main/mesh_task.c's CLI stdout echo, emitted only once a
# message is actually received, decrypted and stored: genuine delivery, not a
# count or the absence of a crash) and stop the instant both are present. The
# budget is sized as a multiple of the nominal schedule, not a duration:
# park-b's fixed 180s mesh delay must exceed both the ~120s worst-case FAILED
# window AND the time both sends are actually parked, because
# mesh_flush_parked_for only fires on an is_new_peer edge and that edge fires
# EXACTLY ONCE per admission (see is_new_peer in mesh_beacon.c); if park-b
# became reachable before both rows were parked, that edge would find nothing
# to flush (confirmed empirically: a 20s mesh delay still let the two sends
# reach their real ~120s FAILED TTL untouched, because nothing rescues a
# route-queued entry just because the destination becomes reachable, but the
# early is_new_peer never re-fires once the rows are later parked, so nothing
# would ever redeliver them). 180s comfortably exceeds that ~120-125s
# park-completion point with margin, plus settle time for the post-rejoin DM
# handshake (session-less, so it needs a fresh KE exchange). 900s gives
# roughly 4x the ~200-220s nominal completion, matching this suite's practice
# of a wide margin for CPU-starvation slack. A genuine regression (flush never
# fires, or the sticky-park guard breaks) simply never produces both lines and
# the wait times out.
#
# NEGATIVE CONTROL (run by hand, not in CI, so the gate stays one run): the
# same scenario with the parking step disabled and everything else identical
# must NOT produce either marker. Verified 2026-08-09 at 86c07401: both DMs
# were sent and neither was ever received, no "[MSG from ...]" line appeared
# at all, and final_metrics reported delivered 0. So the delivery in the
# positive run is attributable to the park plus the rejoin-edge flush and to
# nothing else, rather than to a send that would have succeeded anyway.
parked_suite() {
    echo "[5] emu-parked-delivery"
    local budget_s="${EMU_PARKED_BUDGET_S:-900}"
    local sim_ms="${EMU_PARKED_SIM_CAP_MS:-600000}"
    local scen="$SCEN_DIR/emu-parked-delivery.json"
    [ -f "$scen" ] || { red "scenario missing: $scen"; return 1; }
    local PARKED_LOG; PARKED_LOG="$LOG_DIR/emu-parked-delivery-$(date +%s).log"

    info "running emu-parked-delivery (marker budget ${budget_s}s, sim cap $((sim_ms / 1000))s)..."
    EMU_SCENARIO_DURATION_MS="$sim_ms" \
        timeout "$budget_s" "$GOSIM_BIN" -headless -scenario "$scen" >"$PARKED_LOG" 2>&1 &
    local pid=$!
    CHILD_PIDS+=("$pid")

    # Matches mesh_task.c's "Also print to stdout for CLI users" line
    # ('[MSG from %08X] %s'), emitted only once a message is actually
    # received, decrypted and stored (main/mesh_task.c ~line 1040, and the
    # fragment-reassembly path at ~line 956). Deliberately NOT the ESP_LOGI
    # '>>> %s' decode-log line right above it: gosim's console capture wraps
    # every line as a JSON string, and Go's default JSON encoder HTML-escapes
    # '>' to >, so a literal '>>> ' pattern silently never matches the
    # log on disk. The bracket line has no such characters. The hex address
    # is a wildcard (park-b's real address is only known at runtime, and
    # differs every run), so this cannot match park-a's own outbound
    # 'park-test DM to <addr> (n/2): PARK ALPHA' log line, a different format
    # that never contains '[MSG from '.
    parked_markers_present() {
        grep -qE '\[MSG from [0-9A-Fa-f]+\] PARK ALPHA' "$PARKED_LOG" &&
            grep -qE '\[MSG from [0-9A-Fa-f]+\] PARK BETA' "$PARKED_LOG"
    }

    local seen=1 deadline
    deadline=$(( $(date +%s) + budget_s ))
    while :; do
        if parked_markers_present; then
            seen=0
            break
        fi
        kill -0 "$pid" 2>/dev/null || break
        [ "$(date +%s)" -lt "$deadline" ] || break
        # 3s cadence, matching dm_suite: the check is a cheap grep here (no
        # frame decoding), but a tighter loop would still steal CPU from the
        # very nodes it waits on over a run this long.
        sleep 3
    done

    # Snapshot the scenario window BEFORE any teardown signal: the death rule
    # below judges only lines logged up to this point, so the suite's own
    # kill cannot manufacture a "death" (see check_no_deaths).
    local scenario_lines
    scenario_lines="$(wc -l < "$PARKED_LOG")"

    # Reap by PROCESS GROUP via the recorded pid, then any direct children:
    # never a name-wide pkill (see the cleanup() comment at the top of this
    # file for why that pattern is unsafe with concurrent suites on one host).
    kill "$pid" 2>/dev/null || true
    pkill -P "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true

    # Final check in case the last marker landed in the frames flushed as
    # gosim exited.
    if [ "$seen" -ne 0 ] && parked_markers_present; then
        seen=0
    fi

    # STRICT death rule: deaths fail the run regardless of assertions.
    check_no_deaths "$PARKED_LOG" 2 "emu-parked-delivery" "$scenario_lines" || return 1

    if [ "$seen" -eq 0 ]; then
        green "PASS: emu-parked-delivery: both parked DMs delivered after park-b rejoined"
        return 0
    fi
    red "FAIL: emu-parked-delivery: did not see both parked DMs delivered within ${budget_s}s"
    dump_diagnostics "$PARKED_LOG" "emu-parked-delivery"
    return 1
}

# --- Scenario 6: parked delivery, peer never left --------------------------
# The companion case to Scenario 5: b2-b never leaves the mesh, so the rejoin
# edge (is_new_peer) can only fire once, on first contact, and never again
# while b2-b stays a known neighbor. b2-a and b2-b establish a normal DM
# session first with a SINGLE 'NEVERLEFT SETUP' DM (EMU_AUTO_SEND, resolved
# via the live neighbor table: this scenario needs an ACTIVE session, not an
# absent peer). Once that is confirmed delivered, b2-b drops its own session
# half in-process (EMU_DROP_DM_SESSION_AT_MS, the same deterministic
# construction emu-dm-desync.json uses) while staying fully up and beaconing
# on schedule: an ACK-loss condition, not an outage. b2-a then sends
# EMU_AUTO_SEND2 ('NEVERLEFT PAYLOAD') under its own still-ACTIVE (now stale)
# session; b2-b cannot decrypt it, so b2-a's pending-ack retries
# (MSG_TIER_NORMAL: 3 attempts, exponential backoff from a 2s base) exhaust
# and the row reads MSG_STATUS_FAILED well under a minute in, no route-queue
# TTL involved. EMU_AUTO_PARK=1 parks it, which arms b2-b's still-present
# neighbor entry (main/parked_retry.c's parked_retry_arm): the peer was never
# absent, so nothing but the beacon-armed retry trigger could ever redeliver
# this row. b2-b's failed decrypt also fires its own #138 self-heal re-INIT
# toward b2-a, which heals b2-a's stale session but does NOT rescue the
# payload (b2-a's retries are retransmissions of ciphertext already sealed
# under the dead session), and then b2-a's next beacon received FROM b2-b
# (still a known neighbor, so is_new_peer is false) is recognised as armed
# and resends the parked row over the now-healed session.
#
# TWO PARTS OF THAT ARE CONSTRUCTED, NOT ASSUMED, AND BOTH WERE OBSERVED
# SILENTLY BREAKING THE SCENARIO BEFORE THEY WERE:
#
#   1. The self-heal above only works if b2-a holds an ATTESTATION PIN for
#      b2-b. b2-b's re-INIT is a first-contact INIT with a zero auth tag, and
#      dm_verify_init refuses a zero tag against a known identity by design,
#      so mesh_dm.c's pinned-peer branch is the only route to a heal. The
#      firmware re-attests every 15 minutes, far outside this run, and the
#      single post-boot attestation is a coin flip against the peer's own
#      half-duplex beacon TX: one run lost it, b2-a logged "INIT verify
#      failed" twice, and the flushed payload was encrypted under the dead
#      session and lost. b2-b therefore re-announces on a compressed cadence
#      (EMU_ATTEST_EVERY_MS) and b2-a holds phase 2 until the pin actually
#      exists (EMU_WAIT_PEER_PINNED), so a missing pin fails the gate instead
#      of quietly voiding it.
#   2. Phase 1 is ONE send. b2-b's drop task fires once any incoming DM has
#      landed, so with a phase-1 burst the second copy can still be in flight
#      at the drop; its post-drop arrival becomes the first failed decrypt,
#      the heal completes seconds BEFORE phase 2 is sent, and the payload
#      then goes out over a healthy session and is delivered first try with
#      nothing ever parked. That run still prints the marker, so it passes
#      while proving nothing. Observed directly on a 2-send phase 1.
#
# The wait is EVENT-DRIVEN like the other suites: poll the growing log for
# the '[MSG from <addr>] NEVERLEFT PAYLOAD' line (main/mesh_task.c's CLI
# stdout echo, the same receive-only marker Scenario 5 uses, emitted only on
# a genuine decoded receive) and stop the instant it appears.
#
# BUDGET AS A STARVATION MULTIPLE, not a duration (dm_suite's reasoning).
# Measured nominal completion is 63-72s on an unloaded box: SETUP delivers in
# seconds, PAYLOAD fails after ~36s of ACK-retry exhaustion, and the
# beacon-armed flush lands within ~2s of the park. The critical path is
# mostly CPU and airtime bound (neighbor discovery, the KE handshake, the
# attestation exchange), which is exactly what a loaded runner pod stretches,
# and the historical IO-storm starvation on these pods ran ~4x, so a 4x
# budget would leave no headroom at all. 600s is ~8x the measured schedule,
# the same margin class channel_suite and dm_suite run at. Cost on a healthy
# box is nothing (it still exits at ~70s). The outer budget runs 30s past the
# sim cap so a capped run self-exits and flushes its final lines before
# timeout SIGTERMs it, mirroring channel_suite's 180/150 gap. A genuine
# regression (the beacon-armed trigger never fires, or the sticky-park guard
# breaks) simply never produces the line and the wait times out.
#
# NEGATIVE CONTROL (run by hand, not in CI, so the gate stays one run): the
# same scenario with EMU_AUTO_PARK=0 and everything else identical must NOT
# produce the marker. Verified 2026-08-08 at dc371a31: the control reproduced
# the failure and the self-heal identically ("Message ... failed after 3
# attempts", "re-accepting as first contact"), then sat for the full 300s
# with no park, no flush and no delivery. So the delivery in the positive run
# is attributable to the park plus the beacon-armed flush and to nothing else.
parked_live_suite() {
    echo "[6] emu-parked-delivery-live"
    local budget_s="${EMU_PARKED_LIVE_BUDGET_S:-600}"
    local sim_ms="${EMU_PARKED_LIVE_SIM_CAP_MS:-570000}"
    local scen="$SCEN_DIR/emu-parked-delivery-live.json"
    [ -f "$scen" ] || { red "scenario missing: $scen"; return 1; }
    local LIVE_LOG; LIVE_LOG="$LOG_DIR/emu-parked-delivery-live-$(date +%s).log"

    info "running emu-parked-delivery-live (marker budget ${budget_s}s, sim cap $((sim_ms / 1000))s)..."
    EMU_SCENARIO_DURATION_MS="$sim_ms" \
        timeout "$budget_s" "$GOSIM_BIN" -headless -scenario "$scen" >"$LIVE_LOG" 2>&1 &
    local pid=$!
    CHILD_PIDS+=("$pid")

    live_marker_present() {
        grep -qE '\[MSG from [0-9A-Fa-f]+\] NEVERLEFT PAYLOAD' "$LIVE_LOG"
    }

    local seen=1 deadline
    deadline=$(( $(date +%s) + budget_s ))
    while :; do
        if live_marker_present; then
            seen=0
            break
        fi
        kill -0 "$pid" 2>/dev/null || break
        [ "$(date +%s)" -lt "$deadline" ] || break
        sleep 3
    done

    # Snapshot the scenario window BEFORE any teardown signal (see
    # check_no_deaths).
    local scenario_lines
    scenario_lines="$(wc -l < "$LIVE_LOG")"

    # Reap by PROCESS GROUP via the recorded pid, then any direct children:
    # never a name-wide pkill.
    kill "$pid" 2>/dev/null || true
    pkill -P "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true

    if [ "$seen" -ne 0 ] && live_marker_present; then
        seen=0
    fi

    check_no_deaths "$LIVE_LOG" 2 "emu-parked-delivery-live" "$scenario_lines" || return 1

    if [ "$seen" -eq 0 ]; then
        green "PASS: emu-parked-delivery-live: parked DM delivered via the beacon-armed retry"
        return 0
    fi
    red "FAIL: emu-parked-delivery-live: did not see the parked DM delivered within ${budget_s}s"
    dump_diagnostics "$LIVE_LOG" "emu-parked-delivery-live"
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
location_suite; loc_rc=$?
echo
parked_suite; parked_rc=$?
echo
parked_live_suite; parked_live_rc=$?
echo

[ "$chan_rc" -eq 0 ] || FAILURES=$((FAILURES + 1))
[ "$dm_rc" -eq 0 ] || FAILURES=$((FAILURES + 1))
[ "$gps_rc" -eq 0 ] || FAILURES=$((FAILURES + 1))
[ "$loc_rc" -eq 0 ] || FAILURES=$((FAILURES + 1))
[ "$parked_rc" -eq 0 ] || FAILURES=$((FAILURES + 1))
[ "$parked_live_rc" -eq 0 ] || FAILURES=$((FAILURES + 1))

# --- verdict --------------------------------------------------------------
if [ "$FAILURES" -eq 0 ]; then
    green "=== SCENARIO SUITE PASS ==="
    exit 0
fi
red "=== SCENARIO SUITE FAIL ($FAILURES assertion(s)) ==="
exit 1
