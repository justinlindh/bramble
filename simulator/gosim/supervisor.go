package main

import (
	"bufio"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"time"
)

// firmwareNodeSpec is one declared firmware-node group from a scenario file
// (the "firmware_nodes" array): a binary to run, how many instances, and where
// each sits in the ether.
//
//	{"type": "firmware", "binary": "emulator/node/build/bramble-node",
//	 "count": 3, "positions": [[0,0],[100,0],[50,80]], "label": "pager"}
//
// The broker learns a node's radio identity from its emu-link hello; the
// scenario supplies only the position (per instance, from positions) and how
// many to spawn. env carries optional extra environment for the process on top
// of the always-set NODE_DIR and EMU_BROKER.
type firmwareNodeSpec struct {
	Type      string
	Binary    string
	Count     int
	Positions [][2]float32
	Label     string
	Env       map[string]string
}

// Supervisor spawns and babysits the firmware node processes for a scenario. It
// reserves one broker slot per instance, spawns the binary with a unique
// NODE_DIR (its NVS/identity state, persistent across restarts) and EMU_BROKER
// (the socket to dial), captures its stdout as that node's console stream, and
// restarts the process whenever it exits, which is exactly how the emulated
// reset button works: reset kills the process, the supervisor brings it back.
type Supervisor struct {
	broker  *Broker
	groups  []firmwareNodeSpec
	baseDir string

	mu      sync.Mutex
	procs   []*superProc
	stopped bool
	stopCh  chan struct{}
	wg      sync.WaitGroup
}

// superProc is one supervised instance: its slot, its binary and environment,
// and the currently running command (guarded so Stop can kill it).
type superProc struct {
	sup      *Supervisor
	slot     *extSlot
	nodeType string // "" / "firmware" = linux node; "qemu" = QEMU VM
	binary   string
	nodeDir  string
	env      []string
	label    string

	mu      sync.Mutex
	cmd     *exec.Cmd
	stopped bool
	starts  int
	boundID string // cached emu-link hello id for console tagging (guarded by mu)
}

// maxPendingConsole caps the pre-attach console buffer so a node that boots but
// never sends a hello cannot grow it without bound; past the cap, lines fall
// back to the process label rather than being buffered. Real nodes attach within
// their first handful of log lines, so this is only a memory safety valve.
const maxPendingConsole = 512

// NewSupervisor creates a supervisor for the given firmware groups bound to the
// broker. Per-node state directories are created under a fresh temp base dir.
func NewSupervisor(broker *Broker, groups []firmwareNodeSpec) *Supervisor {
	base, err := os.MkdirTemp("", "bramble-emu-nodes-")
	if err != nil {
		base = filepath.Join(os.TempDir(), fmt.Sprintf("bramble-emu-nodes-%d", os.Getpid()))
		_ = os.MkdirAll(base, 0o755)
	}
	return &Supervisor{
		broker:  broker,
		groups:  groups,
		baseDir: base,
		stopCh:  make(chan struct{}),
	}
}

// Start reserves slots and launches every instance. It returns immediately; the
// instances boot and attach asynchronously (each on its own goroutine), and the
// group is spawned sequentially, waiting for each node to attach before the
// next, so slot-to-position binding is deterministic.
func (s *Supervisor) Start() {
	s.wg.Add(1)
	go s.run()
}

func (s *Supervisor) run() {
	defer s.wg.Done()
	for gi := range s.groups {
		g := s.groups[gi]
		count := g.Count
		if count <= 0 {
			count = 1
		}
		label := g.Label
		if label == "" {
			label = fmt.Sprintf("fw%d", gi)
		}
		for i := 0; i < count; i++ {
			select {
			case <-s.stopCh:
				return
			default:
			}
			x, y := float32(0), float32(0)
			if i < len(g.Positions) {
				x, y = g.Positions[i][0], g.Positions[i][1]
			}
			nodeLabel := fmt.Sprintf("%s-%d", label, i)
			nodeDir := filepath.Join(s.baseDir, nodeLabel)
			_ = os.MkdirAll(nodeDir, 0o755)
			slot := s.broker.reserveSlot(x, y, nodeLabel)

			p := &superProc{
				sup:      s,
				slot:     slot,
				nodeType: g.Type,
				binary:   g.Binary,
				nodeDir:  nodeDir,
				label:    nodeLabel,
				env:      buildNodeEnv(s.broker.Addr(), nodeDir, nodeLabel, g.Env),
			}
			s.mu.Lock()
			if s.stopped {
				s.mu.Unlock()
				return
			}
			s.procs = append(s.procs, p)
			s.mu.Unlock()

			// Drop any attach event that is not this instance's: a restart of
			// an earlier node (or a leftover from a previous scenario) would
			// otherwise satisfy the wait below instantly, letting two node
			// processes race to bind slots. Slot binding is FIFO, so that race
			// permutes positions and the console tagging that reads the slot's
			// bound id, which surfaces one node's log lines under another
			// node's name.
			s.broker.drainAttach()

			s.wg.Add(1)
			go p.supervise()

			// Wait for this instance to attach before spawning the next, so
			// slots bind to instances in declaration order (deterministic
			// positions). Bounded so a node that never attaches cannot wedge
			// startup of the rest.
			s.waitAttach(2 * time.Second)
		}
	}
}

// waitAttach blocks until the broker reports one attach or the timeout elapses.
func (s *Supervisor) waitAttach(timeout time.Duration) {
	select {
	case <-s.broker.attachCh:
	case <-time.After(timeout):
	case <-s.stopCh:
	}
}

// Stop signals every instance to stop and kills any running process.
func (s *Supervisor) Stop() {
	s.mu.Lock()
	if s.stopped {
		s.mu.Unlock()
		return
	}
	s.stopped = true
	close(s.stopCh)
	procs := append([]*superProc(nil), s.procs...)
	s.mu.Unlock()

	for _, p := range procs {
		p.stop()
	}
	s.wg.Wait()
	_ = os.RemoveAll(s.baseDir)
}

// buildNodeEnv assembles the child process environment: the parent environment
// plus the always-set NODE_DIR, EMU_BROKER, and BRAMBLE_EMU_NODE, plus any
// group-specific extras.
func buildNodeEnv(brokerAddr, nodeDir, nodeLabel string, extra map[string]string) []string {
	env := append([]string(nil), os.Environ()...)
	// EMU_BROKER carries a scheme ("unix:/path"), the contract the node's
	// emu_link client parses (emu_link.h, ../../emulator/DESIGN.md section 8).
	// The broker always listens on a unix socket, so the scheme is always unix.
	//
	// BRAMBLE_EMU_NODE is the hello id the node reports. A linux node derives
	// its id from its crypto identity and ignores this; a QEMU node's bridge
	// (bramble_gpspi2.c) has no in-VM identity to read, so it reports this env
	// value as its hello node id (broker slot binding is by position, so it
	// only affects UI / console tagging).
	env = append(env, "NODE_DIR="+nodeDir, "EMU_BROKER=unix:"+brokerAddr,
		"BRAMBLE_EMU_NODE="+nodeLabel)
	for k, v := range extra {
		env = append(env, k+"="+v)
	}
	return env
}

// envLookup returns the value of key in an environment slice ("KEY=value"
// entries), or def if absent. Used to read QEMU_* overrides a scenario passed
// through a group's env map.
func envLookup(env []string, key, def string) string {
	prefix := key + "="
	for _, e := range slices.Backward(env) { // last wins, like the real environ
		if strings.HasPrefix(e, prefix) {
			return e[len(prefix):]
		}
	}
	return def
}

// supervise runs the process, capturing its stdout as the node console, and
// restarts it whenever it exits, until the supervisor is stopped. The
// restart-on-exit loop is the reset-button mechanism.
func (p *superProc) supervise() {
	defer p.sup.wg.Done()
	fastFails := 0
	for {
		select {
		case <-p.sup.stopCh:
			return
		default:
		}
		start := time.Now()
		p.runOnce()
		p.mu.Lock()
		stopped := p.stopped
		p.mu.Unlock()
		if stopped {
			return
		}
		// A healthy node that ran for a while (e.g. reset after real work) is
		// restarted promptly; a process that dies immediately (missing binary,
		// crash-on-boot) is backed off, capped, so it neither hot-loops the log
		// nor stops retrying.
		backoff := 50 * time.Millisecond
		if time.Since(start) < time.Second {
			// min(fastFails, 5) caps the shift at 50<<5 = 1600ms, so the
			// backoff is already bounded below 2s; that is the whole cap.
			fastFails++
			backoff = time.Duration(50<<min(fastFails, 5)) * time.Millisecond
		} else {
			fastFails = 0
		}
		select {
		case <-p.sup.stopCh:
			return
		case <-time.After(backoff):
		}
	}
}

// runOnce launches the node once and blocks until it exits, forwarding each
// stdout line to the node console. The command is built per node type: a linux
// node is the binary run directly; a QEMU node is qemu-system-xtensa driving
// the pager image with the emu-link chardev wired (buildQemuCmd).
func (p *superProc) runOnce() {
	cmd, err := p.buildCmd()
	if err != nil {
		log.Printf("supervisor: %s build command: %v", p.label, err)
		return
	}
	// The child's working directory is left as the launcher's, so a scenario's
	// relative binary path (e.g. "emulator/node/build/bramble-node") resolves
	// against where gosim was started; per-node state lives under NODE_DIR, not
	// the cwd.
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		log.Printf("supervisor: %s stdout pipe: %v", p.label, err)
		return
	}
	cmd.Stderr = cmd.Stdout // fold stderr into the same console stream

	p.mu.Lock()
	if p.stopped {
		p.mu.Unlock()
		return
	}
	if err := cmd.Start(); err != nil {
		p.mu.Unlock()
		log.Printf("supervisor: %s start %q: %v", p.label, p.binary, err)
		return
	}
	p.cmd = cmd
	p.starts++
	p.mu.Unlock()

	sc := bufio.NewScanner(stdout)
	sc.Buffer(make([]byte, 64*1024), 1<<20)
	// Tag each console line with the node's bound emu-link hello id once it has
	// attached, so console events carry the node's real address (not the process
	// label) and route correctly even across multiple firmware groups. Lines seen
	// before the hello are buffered and flushed under that id; if the node never
	// attaches, they fall back to the process label at exit.
	var pending []string
	consoleID := ""
	for sc.Scan() {
		// consoleID() takes the mutex; once the bound id is known it is stable
		// for this run, so resolve it once and stop locking per line.
		if consoleID == "" {
			consoleID = p.consoleID()
		}
		if consoleID == "" {
			if len(pending) < maxPendingConsole {
				pending = append(pending, sc.Text())
			} else {
				p.sup.broker.sim.emitConsole(p.label, sc.Text())
			}
			continue
		}
		if len(pending) > 0 {
			p.flushPending(consoleID, pending)
			pending = nil
		}
		p.sup.broker.sim.emitConsole(consoleID, sc.Text())
	}
	if len(pending) > 0 {
		// Never attached: re-check once for a late bind, else fall back to the
		// process label.
		id := p.consoleID()
		if id == "" {
			id = p.label
		}
		p.flushPending(id, pending)
	}
	werr := cmd.Wait()

	p.mu.Lock()
	p.cmd = nil
	stopped := p.stopped
	p.mu.Unlock()

	// A node dying mid-scenario is invisible unless its exit status is
	// logged: without this, a crash-and-restart can only be inferred from a
	// duplicate attach line, with no evidence of WHY the process died. Log
	// every unexpected exit with its wait error (which carries the exit code
	// or signal); teardown kills (p.stopped) stay silent so normal scenario
	// shutdown does not spray fake death reports. The scenario suite's
	// failure diagnostics grep for these lines.
	if !stopped {
		if werr != nil {
			log.Printf("supervisor: %s exited unexpectedly: %v (restart-on-exit will bring it back)", p.label, werr)
		} else {
			log.Printf("supervisor: %s exited cleanly mid-scenario (code 0); restart-on-exit will bring it back", p.label)
		}
	}
}

// flushPending emits each buffered pre-attach console line under id, in order,
// then the caller clears the buffer. Shared by the in-loop flush (on the line
// where the node's id first resolves) and the post-loop drain (a node that
// exited before attaching).
func (p *superProc) flushPending(id string, pending []string) {
	for _, pl := range pending {
		p.sup.broker.sim.emitConsole(id, pl)
	}
}

// buildCmd assembles the *exec.Cmd for one launch, dispatching on node type.
// A linux node is its binary run directly; a QEMU node is qemu-system-xtensa
// driving the pager image (buildQemuCmd). The environment is set here; the
// caller wires stdout/stderr.
func (p *superProc) buildCmd() (*exec.Cmd, error) {
	var (
		cmd *exec.Cmd
		err error
	)
	if p.nodeType == "qemu" {
		if cmd, err = p.buildQemuCmd(); err != nil {
			return nil, err
		}
	} else {
		cmd = exec.Command(p.binary)
	}
	cmd.Env = p.env
	return cmd, nil
}

// buildQemuCmd builds the qemu-system-xtensa invocation for a QEMU pager node:
// the esp32s3 machine driving a per-node copy of the merged flash + eFuse images
// (so each VM has its own persistent NVS identity, like a linux node's NODE_DIR),
// with the emu-link chardev wired so the in-VM bridge (bramble_gpspi2.c) dials
// the broker. The base images are the ones emulator/qemu/run-qemu.sh assembles
// (build-qemu/flash_qemu.bin + efuse_qemu.bin); the scenario's "binary" field
// carries the flash image path and QEMU_EFUSE (or a sibling efuse_qemu.bin) the
// eFuse image. The guest UART0 rides -nographic onto stdout, so the existing
// console capture keeps working.
func (p *superProc) buildQemuCmd() (*exec.Cmd, error) {
	qemuBin := qemuBinary(p.env)
	if qemuBin == "" {
		return nil, fmt.Errorf("qemu-system-xtensa not found (set QEMU_XTENSA)")
	}
	flashBase := p.binary
	if flashBase == "" {
		return nil, fmt.Errorf("qemu node: empty flash image path (scenario 'binary')")
	}
	efuseBase := envLookup(p.env, "QEMU_EFUSE",
		filepath.Join(filepath.Dir(flashBase), "efuse_qemu.bin"))

	// Per-node image copies: own NVS identity + flash writeback, and a reset
	// (restart) reuses the same NVS so identity persists, exactly like a linux
	// node's NODE_DIR. Copy only if missing.
	flash := filepath.Join(p.nodeDir, "flash.bin")
	efuse := filepath.Join(p.nodeDir, "efuse.bin")
	if err := copyFileIfMissing(flashBase, flash); err != nil {
		return nil, fmt.Errorf("qemu node: flash image %q: %w", flashBase, err)
	}
	if err := copyFileIfMissing(efuseBase, efuse); err != nil {
		return nil, fmt.Errorf("qemu node: efuse image %q: %w", efuseBase, err)
	}

	broker := p.sup.broker.Addr()
	args := []string{
		"-machine", "esp32s3", "-nographic",
		"-drive", "file=" + flash + ",if=mtd,format=raw",
		"-drive", "file=" + efuse + ",if=none,format=raw,id=efuse",
		"-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
		// emu-link transport: QEMU dials the broker's unix listener as a client
		// (server=off); reconnect retries if the socket races broker startup.
		"-chardev", "socket,id=emulink,path=" + broker + ",server=off,reconnect=1",
	}
	return exec.Command(qemuBin, args...), nil
}

// qemuBinary resolves qemu-system-xtensa: an explicit QEMU_XTENSA, else the
// from-source build under ~/src/qemu-esp (bootstrap-qemu.sh's default), else the
// PATH. Returns "" if none is found.
func qemuBinary(env []string) string {
	if q := envLookup(env, "QEMU_XTENSA", ""); q != "" {
		return q
	}
	if home, err := os.UserHomeDir(); err == nil {
		cand := filepath.Join(home, "src", "qemu-esp", "build", "qemu-system-xtensa")
		if st, err := os.Stat(cand); err == nil && !st.IsDir() {
			return cand
		}
	}
	if p, err := exec.LookPath("qemu-system-xtensa"); err == nil {
		return p
	}
	return ""
}

// copyFileIfMissing copies src to dst unless dst already exists (so a restart
// preserves a QEMU node's NVS). Returns any I/O error.
func copyFileIfMissing(src, dst string) error {
	if _, err := os.Stat(dst); err == nil {
		return nil
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

// stop marks the instance stopped and kills any running process.
func (p *superProc) stop() {
	p.mu.Lock()
	p.stopped = true
	cmd := p.cmd
	p.mu.Unlock()
	if cmd != nil && cmd.Process != nil {
		_ = cmd.Process.Kill()
	}
}

// startCount reports how many times this instance has been (re)started, for
// tests asserting restart-on-exit.
func (p *superProc) startCount() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.starts
}

// consoleID returns the emu-link hello id bound to this instance's slot, caching
// it once resolved so console lines emitted after a restart still tag to the
// stable identity before the next hello re-binds the slot. It is "" until the
// first hello attaches (the caller buffers lines until then).
func (p *superProc) consoleID() string {
	p.mu.Lock()
	cached := p.boundID
	p.mu.Unlock()
	if cached != "" {
		return cached
	}
	id := p.sup.broker.slotBoundID(p.slot)
	if id != "" {
		p.mu.Lock()
		p.boundID = id
		p.mu.Unlock()
	}
	return id
}
