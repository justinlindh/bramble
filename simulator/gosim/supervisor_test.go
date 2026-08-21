package main

import (
	"encoding/json"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

// TestMain lets this test binary double as the fake firmware node the
// supervisor spawns (the real bramble-node binary is built by another task).
// When GOSIM_FAKE_NODE is set in the environment, the process runs the fake
// node instead of the test suite: it dials EMU_BROKER, completes the hello
// handshake, records a boot in NODE_DIR (proving the per-node state dir is
// unique and persists across restarts), prints a console line to stdout
// (proving console capture), then exits (proving restart-on-exit).
func TestMain(m *testing.M) {
	if os.Getenv("GOSIM_FAKE_NODE") != "" {
		runFakeNode()
		return
	}
	os.Exit(m.Run())
}

func runFakeNode() {
	nodeDir := os.Getenv("NODE_DIR")
	broker := os.Getenv("EMU_BROKER")

	// Record this boot in the persistent per-node state dir.
	boots := 0
	countPath := filepath.Join(nodeDir, "boots")
	if b, err := os.ReadFile(countPath); err == nil {
		boots, _ = strconv.Atoi(strings.TrimSpace(string(b)))
	}
	boots++
	_ = os.WriteFile(countPath, []byte(strconv.Itoa(boots)), 0o644)

	// Console line captured by the supervisor.
	fmt.Printf("boot=%d NODE_DIR=%s EMU_BROKER=%s\n", boots, nodeDir, broker)
	os.Stdout.Sync()

	// The hello id defaults to the process's NODE_DIR basename (which equals the
	// supervisor's "<label>-<i>" process label), but a test can force a distinct
	// id via GOSIM_FAKE_NODE_ID to prove console lines tag to the bound hello id
	// rather than the process label.
	node := os.Getenv("GOSIM_FAKE_NODE_ID")
	if node == "" {
		node = filepath.Base(nodeDir)
	}

	if broker != "" {
		// EMU_BROKER carries a "unix:" scheme (the real node's contract); the
		// fake node parses it the same way before dialing.
		dialAddr := strings.TrimPrefix(broker, "unix:")
		if c, err := net.Dial("unix", dialAddr); err == nil {
			b, _ := json.Marshal(map[string]any{"t": "hello", "node": node, "version": EmuLinkVersion})
			c.Write(append(b, '\n'))
			// Emit a firmware log line over emu-link too.
			lg, _ := json.Marshal(map[string]any{"t": "log", "line": "fake node online"})
			c.Write(append(lg, '\n'))
			// A post-attach stdout line, so the multi-group test can assert this
			// line tags to the bound hello id (not the process label).
			time.Sleep(50 * time.Millisecond) // let the broker process the hello
			fmt.Printf("attached id=%s\n", node)
			os.Stdout.Sync()
			time.Sleep(150 * time.Millisecond) // stay attached long enough to register
			c.Close()
		}
	}
	os.Exit(0)
}

func TestSupervisorSpawnConsoleAndRestart(t *testing.T) {
	self, err := os.Executable()
	if err != nil {
		t.Fatalf("os.Executable: %v", err)
	}

	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	spec := firmwareNodeSpec{
		Type:      "firmware",
		Binary:    self,
		Count:     1,
		Positions: [][2]float32{{10, 20}},
		Label:     "pager",
		Env:       map[string]string{"GOSIM_FAKE_NODE": "1"},
	}
	sup := NewSupervisor(h.sim.broker, []firmwareNodeSpec{spec})
	sup.Start()
	defer sup.Stop()

	// Let the node boot, attach, and restart a few times (each boot lives
	// ~150 ms, restarts have a 50 ms backoff). Wait on the boot COUNTER, not
	// just startCount: startCount increments when the supervisor initiates the
	// second exec, but the restarted process only writes the boots file after
	// its own runtime starts, and on a loaded runner reading the file in that
	// gap raced to "1" and flaked this test.
	deadline := time.Now().Add(3 * time.Second)
	var proc *superProc
	boots := 0
	for time.Now().Before(deadline) {
		sup.mu.Lock()
		if len(sup.procs) > 0 {
			proc = sup.procs[0]
		}
		sup.mu.Unlock()
		if proc != nil {
			if b, err := os.ReadFile(filepath.Join(proc.nodeDir, "boots")); err == nil {
				boots, _ = strconv.Atoi(strings.TrimSpace(string(b)))
			}
			if boots >= 2 && proc.startCount() >= 2 {
				break
			}
		}
		time.Sleep(50 * time.Millisecond)
	}
	if proc == nil {
		t.Fatal("supervisor never created a process")
	}

	// Restart-on-exit: the process was (re)started at least twice.
	if got := proc.startCount(); got < 2 {
		t.Fatalf("expected >= 2 starts (restart-on-exit), got %d", got)
	}

	// The per-node state dir persisted across restarts (boot counter advanced).
	if boots < 2 {
		t.Fatalf("boot counter = %d, want >= 2 (persistent NODE_DIR across restarts)", boots)
	}

	// Console capture: the supervisor forwarded the node's stdout as console
	// events, and the node attached over emu-link (node_joined + its log line).
	lines := h.snapshot()
	var sawConsole, sawJoin, sawLog bool
	for _, l := range lines {
		if strings.Contains(l, `"type":"console"`) && strings.Contains(l, "boot=") {
			sawConsole = true
		}
		if strings.Contains(l, `"type":"node_joined"`) && strings.Contains(l, `"kind":"firmware"`) {
			sawJoin = true
		}
		if strings.Contains(l, `"type":"console"`) && strings.Contains(l, "fake node online") {
			sawLog = true
		}
	}
	if !sawConsole {
		t.Error("no console line captured from the node's stdout")
	}
	if !sawJoin {
		t.Error("node never attached over emu-link (no firmware node_joined)")
	}
	if !sawLog {
		t.Error("node's emu-link log line was not forwarded to the console")
	}
}

// consoleNodeFor scans the harness's captured events for the console line
// containing marker and returns its "node" tag (the id the broker routed the
// line to), or "" if no such console event was seen.
func consoleNodeFor(lines []string, marker string) string {
	for _, l := range lines {
		var ev struct {
			Type string `json:"type"`
			Node string `json:"node"`
			Line string `json:"line"`
		}
		if err := json.Unmarshal([]byte(l), &ev); err != nil {
			continue
		}
		if ev.Type == "console" && strings.Contains(ev.Line, marker) {
			return ev.Node
		}
	}
	return ""
}

// TestSupervisorConsoleTaggedByBoundIDMultiGroup is the regression guard for the
// resolveDeviceId multi-group mis-routing fix. Two firmware groups ("pager" and
// "gateway") each spawn one instance, so both processes carry a "<label>-0"
// label that ends in "-0": the old label-suffix heuristic mapped BOTH to
// firmwareOrder[0] and cross-wired their consoles. With server-side tagging each
// console line carries the node's bound emu-link hello id instead, so the two
// nodes' consoles stay distinct regardless of label collisions.
func TestSupervisorConsoleTaggedByBoundIDMultiGroup(t *testing.T) {
	self, err := os.Executable()
	if err != nil {
		t.Fatalf("os.Executable: %v", err)
	}

	h := newEmuHarness()
	defer h.close()
	path := filepath.Join(t.TempDir(), "emu.sock")
	if err := h.startBroker(path); err != nil {
		t.Fatal(err)
	}

	// Distinct hello ids that do NOT match the process labels: the whole point is
	// that the console routes by these, not by "pager-0"/"gateway-0".
	const pagerID, gatewayID = "AAAA1111", "BBBB2222"
	specs := []firmwareNodeSpec{
		{
			Type: "firmware", Binary: self, Count: 1,
			Positions: [][2]float32{{0, 0}}, Label: "pager",
			Env: map[string]string{"GOSIM_FAKE_NODE": "1", "GOSIM_FAKE_NODE_ID": pagerID},
		},
		{
			Type: "firmware", Binary: self, Count: 1,
			Positions: [][2]float32{{100, 0}}, Label: "gateway",
			Env: map[string]string{"GOSIM_FAKE_NODE": "1", "GOSIM_FAKE_NODE_ID": gatewayID},
		},
	}
	sup := NewSupervisor(h.sim.broker, specs)
	sup.Start()
	defer sup.Stop()

	// Wait until both nodes have emitted their post-attach console marker.
	deadline := time.Now().Add(5 * time.Second)
	var pagerTag, gatewayTag string
	for time.Now().Before(deadline) {
		lines := h.snapshot()
		pagerTag = consoleNodeFor(lines, "attached id="+pagerID)
		gatewayTag = consoleNodeFor(lines, "attached id="+gatewayID)
		if pagerTag != "" && gatewayTag != "" {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}

	if pagerTag != pagerID {
		t.Errorf("pager console tagged %q, want bound hello id %q", pagerTag, pagerID)
	}
	if gatewayTag != gatewayID {
		t.Errorf("gateway console tagged %q, want bound hello id %q", gatewayTag, gatewayID)
	}
	// No console line may be tagged with a raw process label ("pager-0" /
	// "gateway-0"): that would mean the old label-based routing leaked back in.
	lines := h.snapshot()
	for _, l := range lines {
		var ev struct {
			Type string `json:"type"`
			Node string `json:"node"`
		}
		if json.Unmarshal([]byte(l), &ev) != nil || ev.Type != "console" {
			continue
		}
		if ev.Node == "pager-0" || ev.Node == "gateway-0" {
			t.Errorf("console event tagged with process label %q (should use bound hello id)", ev.Node)
		}
	}
}
