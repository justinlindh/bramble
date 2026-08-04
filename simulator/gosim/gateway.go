package main

// Gateway is the serial-attached PHY-passthrough bridge (DESIGN.md section 10):
// a real hardware node reached over serial whose radio becomes one more member
// of the emulated ether. It speaks two protocols and shuttles frames between
// them:
//
//   - the node's JSON-RPC serial link (the phy.* methods): it enables
//     passthrough (authenticated over serial by physical access), transmits
//     frames the ether hands it (phy.tx), and consumes the node's inbound
//     "bramble.onPhyFrame" notifications (a real-mesh reception plus rssi/snr/
//     freq);
//   - the emu-link ether (DESIGN.md section 8): it attaches to the broker as
//     an ordinary external node (hello/tx/rx), so a frame the hardware hears
//     from the real fleet is injected as this node's transmission (every
//     in-range virtual pager receives it), and a frame the ether routes to the
//     gateway's position goes back out the real radio via phy.tx.
//
// The two directions are symmetric, so a channel message from the physical
// fleet lands on the virtual pagers, and a reply composed on a virtual pager
// reaches the physical fleet. The bridge is deliberately transport-agnostic:
// Run opens the real /dev/ttyUSB* device, while bridge() takes an already-open
// duplex link, which lets the tests drive it over an in-memory pipe.

import (
	"bufio"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/exec"
	"sync"
	"sync/atomic"
	"time"
)

// gatewayTTLSec is the passthrough window the gateway requests. The node
// auto-expires the mode after this (a hard safety property, DESIGN.md section
// 10), so the gateway re-enables at half the interval to keep a long bridging
// session alive without ever leaving the window open longer than one TTL.
const gatewayTTLSec = 1800 // 30 minutes

// gatewayDefaultBaud is the ESP32-S3 console baud (both the V3 CP2102 bridge
// and native USB-serial-JTAG default to this).
const gatewayDefaultBaud = 115200

// Gateway bridges one serial-attached passthrough node into the ether.
type Gateway struct {
	device     string // serial device path (real hardware); empty in tests
	brokerPath string // emu-link unix socket to dial
	nodeName   string // hello id presented to the ether
	baud       int
	force      bool // enable with force:true even if the node holds an identity

	idc uint64 // JSON-RPC id counter

	nodeMu sync.Mutex
	nodeW  io.Writer // serial writer, guarded (phy.tx + keepalive both write)

	etherMu sync.Mutex
	etherW  io.Writer // emu-link writer to the broker
}

// NewGateway returns a gateway that will bridge the node on serial device into
// the ether reachable at the broker's emu-link socket path.
func NewGateway(brokerPath, device string) *Gateway {
	return &Gateway{
		device:     device,
		brokerPath: brokerPath,
		nodeName:   "gateway",
		baud:       gatewayDefaultBaud,
	}
}

func (g *Gateway) nextID() uint64 { return atomic.AddUint64(&g.idc, 1) }

// --- JSON-RPC (serial) wire types ---

type rpcRequest struct {
	JSONRPC string `json:"jsonrpc"`
	ID      uint64 `json:"id"`
	Method  string `json:"method"`
	Params  any    `json:"params,omitempty"`
}

type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

// rpcInbound covers both a response (id + result/error) and a notification
// (method + params); a single decode handles either because the fields are
// disjoint.
type rpcInbound struct {
	ID     uint64          `json:"id"`
	Method string          `json:"method"`
	Result json.RawMessage `json:"result"`
	Error  *rpcError       `json:"error"`
	Params json.RawMessage `json:"params"`
}

// phyEnableResult is the phy.enable result payload (see handle_phy_enable).
type phyEnableResult struct {
	Enabled       bool `json:"enabled"`
	RequiresForce bool `json:"requires_force"`
}

// onPhyFrameParams is the bramble.onPhyFrame notification payload: a raw frame
// the node received on the real channel, hex-encoded, plus its carrier. The
// notification also carries the node's measured rssi/snr, but the bridge does
// not consume them: the ether reprices signal quality per virtual receiver, so
// the gateway's single measured pair has nowhere to go in this model.
type onPhyFrameParams struct {
	Frame string `json:"frame"`
	Freq  int64  `json:"freq"` // carrier in Hz
}

// --- emu-link (ether) wire types (subset; see DESIGN.md section 8) ---

// etherInbound is the broker->node message shape the gateway consumes. Only rx
// drives the bridge; time/txdone/cadres are read and ignored.
type etherInbound struct {
	T       string `json:"t"`
	Payload string `json:"payload"` // rx: base64 PHY payload
	Freq    int64  `json:"freq"`
}

// Run opens the real serial device and bridges until the link closes or an
// error occurs. This is the operator's `--gateway /dev/ttyUSB0` entry point.
func (g *Gateway) Run() error {
	node, err := openSerial(g.device, g.baud)
	if err != nil {
		return fmt.Errorf("gateway: open serial %s: %w", g.device, err)
	}
	defer node.Close()
	return g.bridge(node)
}

// bridge enables passthrough on the node, attaches to the ether, and pumps
// frames both ways until either link drops. node is an already-open duplex
// link (the real serial port, or a test pipe).
func (g *Gateway) bridge(node io.ReadWriteCloser) error {
	g.nodeMu.Lock()
	g.nodeW = node
	g.nodeMu.Unlock()
	nr := bufio.NewReader(node)

	// 1. Enable passthrough on the real node. Over serial the node treats the
	//    caller as authenticated (physical access); it still refuses if it
	//    holds a live identity, so escalate to force once if asked to.
	if err := g.enable(nr, g.force); err != nil {
		return fmt.Errorf("gateway: enable passthrough: %w", err)
	}
	log.Printf("gateway: passthrough enabled on %s", g.deviceLabel())

	// 2. Attach to the ether as one more PHY member.
	ether, err := net.Dial("unix", g.brokerPath)
	if err != nil {
		return fmt.Errorf("gateway: dial broker %s: %w", g.brokerPath, err)
	}
	defer ether.Close()
	g.etherMu.Lock()
	g.etherW = ether
	g.etherMu.Unlock()
	er := bufio.NewReader(ether)
	if err := g.etherHello(er); err != nil {
		return fmt.Errorf("gateway: ether attach: %w", err)
	}
	log.Printf("gateway: attached to ether at %s as %q", g.brokerPath, g.nodeName)

	stop := make(chan struct{})
	var once sync.Once
	done := func() { once.Do(func() { close(stop) }) }
	var wg sync.WaitGroup
	wg.Add(2)

	// serial -> ether: a real-mesh reception becomes an ether transmission.
	go func() {
		defer wg.Done()
		defer done()
		g.pumpSerialToEther(nr)
	}()
	// ether -> serial: a frame routed to this node goes out the real radio.
	go func() {
		defer wg.Done()
		defer done()
		g.pumpEtherToSerial(er)
	}()
	// keepalive re-enable so the node never auto-expires mid-session.
	go g.keepalive(stop)

	<-stop
	node.Close()
	ether.Close()
	wg.Wait()
	return nil
}

// enable sends phy.enable and waits for its response, skipping any interleaved
// notifications. If the node refuses because it holds a live identity, it
// retries once with force:true (the operator opted in by attaching a gateway).
func (g *Gateway) enable(nr *bufio.Reader, force bool) error {
	id := g.nextID()
	if err := g.sendNode(rpcRequest{
		JSONRPC: "2.0", ID: id, Method: "phy.enable",
		Params: map[string]any{"ttl_s": gatewayTTLSec, "force": force},
	}); err != nil {
		return err
	}
	for {
		var m rpcInbound
		line, err := nr.ReadBytes('\n')
		if err != nil {
			return err
		}
		if json.Unmarshal(line, &m) != nil || m.ID != id {
			continue // a notification or an unrelated response
		}
		if m.Error != nil {
			return fmt.Errorf("phy.enable rpc error %d: %s", m.Error.Code, m.Error.Message)
		}
		var res phyEnableResult
		_ = json.Unmarshal(m.Result, &res)
		if res.Enabled {
			g.force = force // remember what worked, for keepalive
			return nil
		}
		if res.RequiresForce && !force {
			return g.enable(nr, true)
		}
		return fmt.Errorf("phy.enable refused (requires_force=%v)", res.RequiresForce)
	}
}

// etherHello performs the emu-link handshake and waits for the broker's time
// acknowledgement, which confirms the gateway has attached and bound a slot.
func (g *Gateway) etherHello(er *bufio.Reader) error {
	if err := g.sendEther(map[string]any{
		"t": "hello", "node": g.nodeName, "version": EmuLinkVersion,
	}); err != nil {
		return err
	}
	for {
		var m etherInbound
		line, err := er.ReadBytes('\n')
		if err != nil {
			return fmt.Errorf("no time ack (broker refused attach?): %w", err)
		}
		if json.Unmarshal(line, &m) == nil && m.T == "time" {
			return nil
		}
	}
}

// pumpSerialToEther forwards every bramble.onPhyFrame notification from the
// node into the ether as this gateway's transmission.
func (g *Gateway) pumpSerialToEther(nr *bufio.Reader) {
	for {
		line, err := nr.ReadBytes('\n')
		if err != nil {
			return
		}
		var m rpcInbound
		if json.Unmarshal(line, &m) != nil || m.Method != "bramble.onPhyFrame" {
			continue // response or unrelated notification
		}
		var p onPhyFrameParams
		if json.Unmarshal(m.Params, &p) != nil {
			continue
		}
		raw, err := hex.DecodeString(p.Frame)
		if err != nil || len(raw) == 0 {
			continue
		}
		// Inject as a broadcast transmission from the gateway's position; the
		// broker prices airtime and delivers to every in-range virtual node.
		if err := g.sendEther(map[string]any{
			"t": "tx", "payload": base64.StdEncoding.EncodeToString(raw),
			"freq": p.Freq, "sf": 10, "bw": 125000, "cr": 1, "power": 22,
		}); err != nil {
			return
		}
	}
}

// pumpEtherToSerial transmits every ether frame routed to the gateway out the
// real radio via phy.tx.
func (g *Gateway) pumpEtherToSerial(er *bufio.Reader) {
	for {
		line, err := er.ReadBytes('\n')
		if err != nil {
			return
		}
		var m etherInbound
		if json.Unmarshal(line, &m) != nil || m.T != "rx" {
			continue
		}
		raw, err := base64.StdEncoding.DecodeString(m.Payload)
		if err != nil || len(raw) == 0 {
			continue
		}
		if err := g.sendNode(rpcRequest{
			JSONRPC: "2.0", ID: g.nextID(), Method: "phy.tx",
			Params: map[string]any{"frame": hex.EncodeToString(raw)},
		}); err != nil {
			return
		}
	}
}

// keepalive re-enables passthrough before the node's TTL lapses, reusing the
// force state that the initial enable settled on.
func (g *Gateway) keepalive(stop <-chan struct{}) {
	tick := time.NewTicker((gatewayTTLSec / 2) * time.Second)
	defer tick.Stop()
	for {
		select {
		case <-stop:
			return
		case <-tick.C:
			_ = g.sendNode(rpcRequest{
				JSONRPC: "2.0", ID: g.nextID(), Method: "phy.enable",
				Params: map[string]any{"ttl_s": gatewayTTLSec, "force": g.force},
			})
		}
	}
}

// writeLine marshals v to a newline-terminated JSON line and writes it to w
// under mu. link names the destination for the not-open error. Both serial and
// ether writers are shared by concurrent writers, so the lock guards every
// write.
func writeLine(mu *sync.Mutex, w io.Writer, link string, v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	b = append(b, '\n')
	mu.Lock()
	defer mu.Unlock()
	if w == nil {
		return fmt.Errorf("gateway: %s link not open", link)
	}
	_, err = w.Write(b)
	return err
}

// sendNode writes one JSON-RPC line to the serial link (guarded: phy.tx and
// keepalive both write).
func (g *Gateway) sendNode(v any) error {
	return writeLine(&g.nodeMu, g.nodeW, "node", v)
}

// sendEther writes one emu-link line to the broker (guarded).
func (g *Gateway) sendEther(v any) error {
	return writeLine(&g.etherMu, g.etherW, "ether", v)
}

func (g *Gateway) deviceLabel() string {
	if g.device != "" {
		return g.device
	}
	return "serial"
}

// openSerial puts the port into raw mode at baud and opens it read/write.
// stty keeps this dependency-free (no cgo/termios, no external Go module); it
// is exercised only on the operator's bench, never by the tests, which inject
// an in-memory link into bridge() directly.
func openSerial(device string, baud int) (io.ReadWriteCloser, error) {
	cmd := exec.Command("stty", "-F", device, "raw", "-echo", "cs8", "-cstopb", "-parenb",
		fmt.Sprint(baud))
	if out, err := cmd.CombinedOutput(); err != nil {
		return nil, fmt.Errorf("stty %s: %v (%s)", device, err, out)
	}
	f, err := os.OpenFile(device, os.O_RDWR, 0)
	if err != nil {
		return nil, err
	}
	return f, nil
}

// RunGatewaySupervised runs the gateway forever, reconnecting after any drop
// (the broker socket may not exist until a firmware scenario loads, and the
// serial link can bounce when the bench node reboots). Intended to be launched
// as a goroutine from main when --gateway is set.
func RunGatewaySupervised(device, brokerPath string) {
	for {
		g := NewGateway(brokerPath, device)
		if err := g.Run(); err != nil {
			log.Printf("gateway: %v (retrying in 2s)", err)
		}
		time.Sleep(2 * time.Second)
	}
}
