package main

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"strconv"
	"strings"
)

// runScreenAssert implements the `screen-assert` subcommand: it replays a
// headless gosim event log (one JSON object per line, as written to stdout by a
// scenario run) and asserts that an expected string was rendered on one or more
// pager screens. It is the headless, OCR-free "the message shows on the panel"
// check the scenario suite (emulator/ci/run_scenarios.sh) gates on.
//
// A screen "contains" the text if ANY device_fb frame for the node in the log
// matches (screenContains), not just the final frame: a message that renders and
// is later scrolled away still counts as delivered-and-shown.
//
// Node selection (exactly one):
//
//	-node ID       assert the node whose emu-link hello id is ID
//	-at X,Y        assert the node that joined at position X,Y (node_joined)
//	-min-nodes N   assert that at least N DISTINCT nodes rendered the text
//
// Exit 0 on success, 1 on a failed assertion, 2 on a usage/IO error.
func runScreenAssert(args []string) int {
	fs := flag.NewFlagSet("screen-assert", flag.ContinueOnError)
	logPath := fs.String("log", "", "headless gosim event log to scan (JSON lines)")
	text := fs.String("text", "", "expected string rendered on the panel")
	node := fs.String("node", "", "assert the node with this emu-link hello id")
	at := fs.String("at", "", "assert the node that joined at position X,Y")
	minNodes := fs.Int("min-nodes", 0, "assert at least N distinct nodes render the text")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if *logPath == "" || *text == "" {
		fmt.Fprintln(os.Stderr, "screen-assert: -log and -text are required")
		return 2
	}
	selectors := 0
	for _, s := range []bool{*node != "", *at != "", *minNodes > 0} {
		if s {
			selectors++
		}
	}
	if selectors != 1 {
		fmt.Fprintln(os.Stderr, "screen-assert: exactly one of -node, -at, -min-nodes is required")
		return 2
	}

	frames, joins, err := loadDeviceFrames(*logPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "screen-assert: %v\n", err)
		return 2
	}

	// Resolve -at to a concrete node id via the node_joined records.
	target := *node
	if *at != "" {
		id, ok := joins[*at]
		if !ok {
			fmt.Fprintf(os.Stderr, "screen-assert: no node joined at %q\n", *at)
			return 1
		}
		target = id
	}

	if *minNodes > 0 {
		hits := map[string]bool{}
		for id, fbs := range frames {
			for _, fb := range fbs {
				if screenContains(fb, *text) {
					hits[id] = true
					break
				}
			}
		}
		if len(hits) >= *minNodes {
			fmt.Printf("PASS: %q rendered on %d node(s): %s\n", *text, len(hits), strings.Join(keys(hits), " "))
			return 0
		}
		fmt.Fprintf(os.Stderr, "FAIL: %q rendered on %d node(s), want >= %d\n", *text, len(hits), *minNodes)
		return 1
	}

	for _, fb := range frames[target] {
		if screenContains(fb, *text) {
			fmt.Printf("PASS: %q rendered on node %s\n", *text, target)
			return 0
		}
	}
	fmt.Fprintf(os.Stderr, "FAIL: %q not found on node %s (%d frames scanned)\n", *text, target, len(frames[target]))
	return 1
}

// loadDeviceFrames scans a headless log and returns, per node id, every decoded
// framebuffer it emitted, plus a map from "X,Y" position to the node id that
// joined there. Positions are formatted with %g so a scenario's integer
// coordinates round-trip ("100,0", not "100.0,0.0").
func loadDeviceFrames(path string) (frames map[string][][]byte, joins map[string]string, err error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, nil, err
	}
	defer f.Close()

	frames = map[string][][]byte{}
	joins = map[string]string{}
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 8<<20)
	for sc.Scan() {
		line := sc.Bytes()
		if !strings.HasPrefix(strings.TrimSpace(string(line)), "{") {
			continue
		}
		var ev struct {
			Type string  `json:"type"`
			Node string  `json:"node"`
			FB   string  `json:"fb"`
			X    float64 `json:"x"`
			Y    float64 `json:"y"`
		}
		if json.Unmarshal(line, &ev) != nil {
			continue
		}
		switch ev.Type {
		case "device_fb":
			if ev.Node == "" || ev.FB == "" {
				continue
			}
			raw, derr := base64.StdEncoding.DecodeString(ev.FB)
			if derr != nil || len(raw) < fbSize {
				continue
			}
			frames[ev.Node] = append(frames[ev.Node], raw)
		case "node_joined":
			if ev.Node != "" {
				joins[posKey(ev.X, ev.Y)] = ev.Node
			}
		}
	}
	return frames, joins, sc.Err()
}

func posKey(x, y float64) string {
	return strconv.FormatFloat(x, 'g', -1, 64) + "," + strconv.FormatFloat(y, 'g', -1, 64)
}

func keys(m map[string]bool) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	return out
}
