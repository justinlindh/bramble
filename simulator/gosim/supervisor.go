package main

import (
	"bufio"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
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
	sup     *Supervisor
	slot    *extSlot
	binary  string
	nodeDir string
	env     []string
	label   string

	mu      sync.Mutex
	cmd     *exec.Cmd
	stopped bool
	starts  int
}

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
			slot := s.broker.reserveSlot(x, y, nodeLabel, nodeDir)

			p := &superProc{
				sup:     s,
				slot:    slot,
				binary:  g.Binary,
				nodeDir: nodeDir,
				label:   nodeLabel,
				env:     buildNodeEnv(s.broker.Addr(), nodeDir, g.Env),
			}
			s.mu.Lock()
			if s.stopped {
				s.mu.Unlock()
				return
			}
			s.procs = append(s.procs, p)
			s.mu.Unlock()

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
// plus the always-set NODE_DIR and EMU_BROKER, plus any group-specific extras.
func buildNodeEnv(brokerAddr, nodeDir string, extra map[string]string) []string {
	env := append([]string(nil), os.Environ()...)
	// EMU_BROKER carries a scheme ("unix:/path"), the contract the node's
	// emu_link client parses (emu_link.h, DESIGN.md section 8). The broker
	// always listens on a unix socket, so the scheme is always unix.
	env = append(env, "NODE_DIR="+nodeDir, "EMU_BROKER=unix:"+brokerAddr)
	for k, v := range extra {
		env = append(env, k+"="+v)
	}
	return env
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
			fastFails++
			backoff = time.Duration(50<<min(fastFails, 5)) * time.Millisecond
			if backoff > 2*time.Second {
				backoff = 2 * time.Second
			}
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

// runOnce launches the binary once and blocks until it exits, forwarding each
// stdout line to the node console.
func (p *superProc) runOnce() {
	cmd := exec.Command(p.binary)
	cmd.Env = p.env
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
	for sc.Scan() {
		p.sup.broker.sim.emitConsole(p.label, sc.Text())
	}
	_ = cmd.Wait()

	p.mu.Lock()
	p.cmd = nil
	p.mu.Unlock()
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
