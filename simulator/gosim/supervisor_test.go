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

	if broker != "" {
		// EMU_BROKER carries a "unix:" scheme (the real node's contract); the
		// fake node parses it the same way before dialing.
		dialAddr := strings.TrimPrefix(broker, "unix:")
		if c, err := net.Dial("unix", dialAddr); err == nil {
			node := filepath.Base(nodeDir)
			b, _ := json.Marshal(map[string]any{"t": "hello", "node": node, "version": EmuLinkVersion})
			c.Write(append(b, '\n'))
			// Emit a firmware log line over emu-link too.
			lg, _ := json.Marshal(map[string]any{"t": "log", "line": "fake node online"})
			c.Write(append(lg, '\n'))
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
	// ~150 ms, restarts have a 50 ms backoff).
	deadline := time.Now().Add(3 * time.Second)
	var proc *superProc
	for time.Now().Before(deadline) {
		sup.mu.Lock()
		if len(sup.procs) > 0 {
			proc = sup.procs[0]
		}
		sup.mu.Unlock()
		if proc != nil && proc.startCount() >= 2 {
			break
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
	countPath := filepath.Join(proc.nodeDir, "boots")
	b, err := os.ReadFile(countPath)
	if err != nil {
		t.Fatalf("read boot counter: %v", err)
	}
	boots, _ := strconv.Atoi(strings.TrimSpace(string(b)))
	if boots < 2 {
		t.Fatalf("boot counter = %d, want >= 2 (persistent NODE_DIR across restarts)", boots)
	}

	// Console capture: the supervisor forwarded the node's stdout as console
	// events, and the node attached over emu-link (node_joined + its log line).
	h.mu.Lock()
	lines := append([]string(nil), h.lines...)
	h.mu.Unlock()
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
