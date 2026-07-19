package main

// Adversarial-scenario gate (issue #95).
//
// simulator/scenarios/ carries 32 hand-maintained JSON scenarios. Until now
// emulator/ci/run_scenarios.sh gated three of them and exactly one gosim test
// referenced a scenarios/*.json path at all, so the adversarial ones (the mesh
// failure modes host unit tests cannot reach: partition, black hole, route
// loop) were maintained but never executed and rotted silently.
//
// This file runs a curated subset in-process, through the same
// runScenarioHeadless entry point the rest of the package uses, and asserts the
// invariant each scenario exists to check. gosim's own CI job already runs
// `go test -count=1 ./...`, so these gate on every PR with no workflow change.
//
// Determinism: every scenario gated here declares "mode": "deterministic" and
// runs on virtual time with a fixed RNG seed and identity keys derived from the
// node id, so a run is reproducible bit for bit. There are deliberately no
// retries, no sleeps, and no tolerance windows: per CLAUDE.md, a scenario that
// only passes sometimes is a bug to fix, not a knob to tune.
//
// WHAT IS NOT GATED HERE, AND WHY (see issue #144):
//
//   - anomaly-partition's heal phase. The scenario kills bridge node C, then
//     re-joins it at its original coordinates to prove the mesh reconverges.
//     Two simulator harness bugs make that phase untestable today:
//     sim_scenario.c's node_join parser never reads the event's "x"/"y" (so a
//     rejoining node always lands at 0,0 regardless of the JSON), and
//     node_array_add appends a second entry with a duplicate id and address
//     instead of reusing the existing one, leaving node_array_find_by_id
//     resolving every by-id lookup to the deactivated corpse. The pre-heal
//     phases below are unaffected and are gated.
//   - anomaly-black-hole entirely. It never establishes its pre-kill baseline
//     (its A->E and E->A sends are 5s apart and collide), never delivers a
//     single message, and never fires the black_hole anomaly it is named for,
//     on top of hitting the same rejoin bugs.
//   - anomaly-route-loop entirely. With the rejoin bugs it is inert: zero DATA
//     packets are ever originated, control traffic is 100% of airtime, so any
//     "no loops were detected" assertion would pass vacuously.
//
// Gating a vacuous assertion is worse than gating nothing, so those stay out
// until #144 is fixed, at which point they should be added here.

import (
	"encoding/json"
	"fmt"
	"sort"
	"strings"
	"testing"
)

// scenarioRun is a decoded headless run of a scenario file: every emitted JSON
// event, plus the node-id to address mapping the scenario's own node_joined
// events establish.
type scenarioRun struct {
	events []map[string]interface{}
	// addrOf maps a scenario node id ("A") to its derived address string
	// ("0x0C57406A"). Resolved from the run rather than hardcoded so that a
	// change to gosim's identity-derived addressing does not silently turn
	// these assertions into address-literal trivia.
	addrOf map[string]string
	// idOf is addrOf inverted, for rendering paths back as node ids.
	idOf map[string]string
}

// runGatedScenario executes simulator/scenarios/<name>.json headlessly and
// decodes its event stream.
func runGatedScenario(t *testing.T, name string) *scenarioRun {
	t.Helper()
	path := "../scenarios/" + name + ".json"
	result, err := runScenarioHeadless(path)
	if err != nil {
		t.Fatalf("runScenarioHeadless(%s): %v", path, err)
	}
	run := &scenarioRun{
		addrOf: map[string]string{},
		idOf:   map[string]string{},
	}
	for _, line := range result.Lines() {
		var evt map[string]interface{}
		if json.Unmarshal([]byte(line), &evt) != nil {
			continue // non-JSON log noise on the captured stream
		}
		run.events = append(run.events, evt)
		if evt["type"] == "node_joined" {
			id, _ := evt["node"].(string)
			addr, _ := evt["addr"].(string)
			if id != "" && addr != "" {
				if _, seen := run.addrOf[id]; !seen {
					run.addrOf[id] = addr
					run.idOf[addr] = id
				}
			}
		}
	}
	if len(run.events) == 0 {
		t.Fatalf("%s: no events emitted", name)
	}
	return run
}

// addr returns the derived address of scenario node id, failing the test if the
// scenario never announced it.
func (r *scenarioRun) addr(t *testing.T, id string) string {
	t.Helper()
	a, ok := r.addrOf[id]
	if !ok {
		t.Fatalf("scenario never emitted a node_joined for node %q", id)
	}
	return a
}

// anomalies returns every emitted anomaly of the given anomaly_type.
func (r *scenarioRun) anomalies(anomalyType string) []map[string]interface{} {
	var out []map[string]interface{}
	for _, e := range r.events {
		if e["type"] == "anomaly" && e["anomaly_type"] == anomalyType {
			out = append(out, e)
		}
	}
	return out
}

// finalMetrics returns the single terminal final_metrics event.
func (r *scenarioRun) finalMetrics(t *testing.T) map[string]interface{} {
	t.Helper()
	for i := len(r.events) - 1; i >= 0; i-- {
		if r.events[i]["type"] == "final_metrics" {
			return r.events[i]
		}
	}
	t.Fatal("no final_metrics event emitted")
	return nil
}

// receipt is a delivery receipt that made it back to the originator: the
// message_delivered event carrying the return path.
type receipt struct {
	packetID string
	// from is the address of the node that sent the receipt, i.e. the original
	// message's destination.
	from string
	// to is the scenario node id of the originator the receipt returned to,
	// i.e. the original message's sender. The emitted path is ordered starting
	// from the hop adjacent to this node.
	to   string
	hops int
	// path is the return path rendered as scenario node ids, in emitted order.
	path []string
}

// receipts returns every delivery receipt that returned to its originator,
// keyed in emission order. A message_delivered event without a "path" is the
// destination-reach half of the exchange, not a returned receipt.
func (r *scenarioRun) receipts() []receipt {
	var out []receipt
	for _, e := range r.events {
		if e["type"] != "message_delivered" {
			continue
		}
		raw, ok := e["path"].([]interface{})
		if !ok {
			continue
		}
		rec := receipt{}
		rec.packetID, _ = e["packet_id"].(string)
		rec.from, _ = e["from"].(string)
		rec.to, _ = e["node"].(string)
		if h, ok := e["hops"].(float64); ok {
			rec.hops = int(h)
		}
		for _, p := range raw {
			addr, _ := p.(string)
			if id, known := r.idOf[addr]; known {
				rec.path = append(rec.path, id)
			} else {
				rec.path = append(rec.path, addr)
			}
		}
		out = append(out, rec)
	}
	return out
}

// findReceipt returns the receipt for packetID, if one returned.
func (r *scenarioRun) findReceipt(packetID string) (receipt, bool) {
	for _, rec := range r.receipts() {
		if rec.packetID == packetID {
			return rec, true
		}
	}
	return receipt{}, false
}

// deliveredAt returns the timestamps at which packetID was reported delivered.
func (r *scenarioRun) deliveredAt(packetID string) []uint64 {
	var out []uint64
	for _, e := range r.events {
		if e["type"] != "message_delivered" || e["packet_id"] != packetID {
			continue
		}
		if ts, ok := e["timestamp_us"].(float64); ok {
			out = append(out, uint64(ts))
		}
	}
	return out
}

// messagesSentBy returns the packet ids every message_sent event attributes to
// node id, in emission order.
func (r *scenarioRun) messagesSentBy(nodeID string) []string {
	var out []string
	for _, e := range r.events {
		if e["type"] == "message_sent" && e["node"] == nodeID {
			if pid, ok := e["packet_id"].(string); ok {
				out = append(out, pid)
			}
		}
	}
	return out
}

// assertNoRoutingPathologies asserts the two anomalies that always indicate a
// broken unicast forwarding plane, whatever else the scenario is testing.
//
// route_loop is asserted only for scenarios whose traffic is unicast. The
// detector (sim_anomaly.c anomaly_check_loop, called from bridge.c on every
// packet RECEIVE) flags any packet id a node sees twice, which is normal and
// expected for a flood: public-channel-broadcast trips it 11 times without any
// actual loop. That detector false positive is reported in issue #144 and is
// why no flooded scenario is gated on it here.
func assertNoRoutingPathologies(t *testing.T, run *scenarioRun) {
	t.Helper()
	for _, a := range run.anomalies("route_loop") {
		t.Errorf("route_loop anomaly fired on unicast traffic: node=%v details=%v at t=%vus",
			a["node"], a["details"], a["timestamp_us"])
	}
	for _, a := range run.anomalies("black_hole") {
		t.Errorf("black_hole anomaly fired: node=%v details=%v at t=%vus",
			a["node"], a["details"], a["timestamp_us"])
	}
}

// TestScenarioAnomalyPartition gates simulator/scenarios/anomaly-partition.json.
//
// The scenario is a 5-node line A-B-C-D-E with a 150-unit radio range and
// 100-unit spacing, so C is the sole bridge between {A,B} and {D,E}. It has
// three phases; the first two are gated here and the third (heal) is blocked by
// issue #144, see this file's header.
//
// Phase 1 (connectivity): before anything is killed, A->E and E->A must each
// traverse the full 4-hop line and have their delivery receipt return to the
// originator over the exact reverse path. This is the baseline the partition
// phase is only meaningful against: without it, "nothing was delivered after
// the partition" would prove nothing.
//
// Phase 2 (partition): killing C must partition the mesh, be detected as such
// naming exactly the nodes that became unreachable, and every message sent
// while the mesh is split must fail to reach the far side.
func TestScenarioAnomalyPartition(t *testing.T) {
	run := runGatedScenario(t, "anomaly-partition")

	a := run.addr(t, "A")
	e := run.addr(t, "E")

	// --- Phase 1: the intact 5-node line delivers both ways, confirmed. ---
	//
	// The scenario scripts exactly two pre-partition sends (at 8s and 18s),
	// A->E then E->A. Both must reach their destination AND have the receipt
	// return across all three intermediate hops.
	sentByA := run.messagesSentBy("A")
	sentByE := run.messagesSentBy("E")
	if len(sentByA) == 0 || len(sentByE) == 0 {
		t.Fatalf("scenario originated no messages from A (%d) or E (%d); the run is inert and "+
			"any partition assertion below would pass vacuously", len(sentByA), len(sentByE))
	}

	aToE := sentByA[0] // the 8s A->E send
	eToA := sentByE[0] // the 18s E->A send

	for _, tc := range []struct {
		label    string
		packetID string
		from     string
		wantPath []string
	}{
		// A's message reaches E; E's receipt walks home D -> C -> B.
		{"A->E (pre-partition)", aToE, e, []string{"B", "C", "D"}},
		// E's message reaches A; A's receipt walks home B -> C -> D.
		{"E->A (pre-partition)", eToA, a, []string{"D", "C", "B"}},
	} {
		rec, ok := run.findReceipt(tc.packetID)
		if !ok {
			t.Errorf("%s (packet %s): no delivery receipt returned to the originator; the "+
				"intact 5-node line must confirm end to end before the partition phase means "+
				"anything", tc.label, tc.packetID)
			continue
		}
		if rec.from != tc.from {
			t.Errorf("%s: receipt from = %s, want %s", tc.label, rec.from, tc.from)
		}
		if rec.hops != len(tc.wantPath) {
			t.Errorf("%s: receipt hops = %d, want %d (the line has exactly three intermediate "+
				"hops)", tc.label, rec.hops, len(tc.wantPath))
		}
		if strings.Join(rec.path, ",") != strings.Join(tc.wantPath, ",") {
			t.Errorf("%s: receipt path = %v, want %v (the only route across the line)",
				tc.label, rec.path, tc.wantPath)
		}
	}

	// --- Phase 2: killing the sole bridge partitions the mesh, detectably. ---
	//
	// C at (200,0) is the only node within radio range of both {A,B} and
	// {D,E}. Killing it must strand exactly D and E relative to the component
	// the detector's BFS starts from, and the detector must say so.
	parts := run.anomalies("mesh_partition")
	if len(parts) != 1 {
		t.Fatalf("mesh_partition anomalies = %d, want exactly 1 (killing the sole bridge C is "+
			"the one partition this scenario creates); got %v", len(parts), parts)
	}
	details, _ := parts[0]["details"].(string)
	const wantDetails = "mesh partitioned: 2 nodes unreachable [D,E]"
	if details != wantDetails {
		t.Errorf("mesh_partition details = %q, want %q (killing C must strand exactly the far "+
			"side of the line)", details, wantDetails)
	}
	if node, _ := parts[0]["node"].(string); node != "network" {
		t.Errorf("mesh_partition node = %q, want \"network\" (a partition is a mesh-wide "+
			"property, not a per-node one)", node)
	}

	// Every message the scenario sends while C is down (three of them, at 43s,
	// 53s and 63s) must fail to cross: a partitioned mesh that still delivers
	// would mean the radio range model is not being enforced.
	partitionedSends := append(append([]string{}, sentByA[1:]...), sentByE[1:]...)
	if len(partitionedSends) == 0 {
		t.Fatal("scenario originated no messages after the partition; nothing to assert")
	}
	for _, pid := range partitionedSends {
		if at := run.deliveredAt(pid); len(at) > 0 {
			t.Errorf("packet %s was delivered at %v despite the mesh being partitioned by C's "+
				"death; nothing may cross a partition", pid, at)
		}
	}

	// The scenario's terminal accounting must agree: exactly the two
	// pre-partition messages are confirmed, and nothing else is.
	fm := run.finalMetrics(t)
	if confirmed, _ := fm["confirmed"].(float64); confirmed != 2 {
		t.Errorf("confirmed = %v, want 2 (only the two pre-partition messages can confirm)",
			fm["confirmed"])
	}
	if delivered, _ := fm["delivered"].(float64); delivered != 2 {
		t.Errorf("delivered = %v, want 2 (only the two pre-partition messages can reach their "+
			"destination)", fm["delivered"])
	}

	assertNoRoutingPathologies(t, run)
}

// TestScenarioReliabilityPathTrace gates
// simulator/scenarios/reliability-path-trace.json, the scenario that exists to
// prove multi-hop unicast picks the correct path and reports it accurately in
// the returned delivery receipt.
//
// Same 5-node line as above with no kills, so it is unaffected by issue #144.
// The invariant is exact route correctness, not a delivery-rate threshold:
// every receipt that returns must have walked the unique shortest path along
// the line between its endpoints, with a hop count that matches that path's
// length. A tolerance-free assertion like this is the point, since a routing
// regression that starts picking longer paths still delivers and would sail
// past any "delivery rate above N" check.
func TestScenarioReliabilityPathTrace(t *testing.T) {
	run := runGatedScenario(t, "reliability-path-trace")

	// The line is A-B-C-D-E at 100-unit spacing. For a receipt returning to an
	// originator from `from`, the path is the strictly-between nodes, ordered
	// from the receipt sender's side back toward the originator.
	line := []string{"A", "B", "C", "D", "E"}
	indexOf := map[string]int{}
	for i, id := range line {
		indexOf[id] = i
	}

	recs := run.receipts()
	if len(recs) == 0 {
		t.Fatal("no delivery receipt returned to any originator; this scenario exists to trace " +
			"return paths, so a run with none is inert rather than passing")
	}

	for _, rec := range recs {
		// The receipt travelled from the original destination (rec.from) back
		// to the originator (rec.to). The emitted path lists the intermediate
		// hops starting from the one adjacent to the originator, so the
		// expected path is simply the nodes strictly between them, walked from
		// the originator's end.
		srcID, sok := indexOf[rec.to]
		dstID, dok := indexOf[run.idOf[rec.from]]
		if !sok || !dok {
			t.Errorf("receipt %s references nodes outside the line A-B-C-D-E: to=%s from=%s",
				rec.packetID, rec.to, rec.from)
			continue
		}
		step := 1
		if dstID < srcID {
			step = -1
		}
		var want []string
		for i := srcID + step; i != dstID; i += step {
			want = append(want, line[i])
		}
		if strings.Join(rec.path, ",") != strings.Join(want, ",") {
			t.Errorf("receipt %s (%s -> %s): return path = %v, want %v; on a line topology the "+
				"only correct route is the direct walk between the endpoints, so a differing "+
				"path means routing chose a detour or reported it wrong",
				rec.packetID, rec.to, run.idOf[rec.from], rec.path, want)
		}
		// hops must equal the number of intermediate nodes actually listed.
		if rec.hops != len(rec.path) {
			t.Errorf("receipt %s: hops = %d but path lists %d intermediate nodes (%v); the "+
				"reported hop count must match the reported path",
				rec.packetID, rec.hops, len(rec.path), rec.path)
		}
	}

	// The scenario must actually exercise multi-hop forwarding, not just
	// single-hop neighbours, or the path assertions above are trivial.
	multiHop := 0
	for _, rec := range recs {
		if rec.hops >= 2 {
			multiHop++
		}
	}
	if multiHop == 0 {
		t.Errorf("no receipt traversed two or more intermediate hops (%d receipts total); "+
			"this scenario must exercise multi-hop forwarding for its path assertions to mean "+
			"anything", len(recs))
	}

	assertNoRoutingPathologies(t, run)
}

// TestGatedScenariosAreDeterministic re-runs every gated scenario in-process
// and requires the emitted event stream to be identical across runs.
//
// This is the property CLAUDE.md cares most about for the scenario suite: the
// repo deliberately removed a 2x re-roll from emulator/ci/run_scenarios.sh
// rather than tune it, on the grounds that these scenarios are deterministic by
// construction. This test makes that claim executable, so a change that
// introduces run-to-run variance fails here rather than showing up later as an
// intermittently red gate.
func TestGatedScenariosAreDeterministic(t *testing.T) {
	for _, name := range []string{"anomaly-partition", "reliability-path-trace"} {
		t.Run(name, func(t *testing.T) {
			const runs = 5
			var reference string
			for i := 0; i < runs; i++ {
				run := runGatedScenario(t, name)
				fingerprint := fingerprintRun(run)
				if i == 0 {
					reference = fingerprint
					continue
				}
				if fingerprint != reference {
					t.Fatalf("run %d diverged from run 0.\n--- run 0 ---\n%s\n--- run %d ---\n%s",
						i, reference, i, fingerprint)
				}
			}
		})
	}
}

// fingerprintRun renders the protocol-visible outcome of a run as a stable
// string: every message lifecycle event and anomaly, with the virtual timestamp
// at which it happened.
//
// Deliberately narrower than "every emitted line". Periodic metrics ticks carry
// float rates whose textual rendering is uninteresting here; the point of the
// fingerprint is that the mesh made the same decisions at the same virtual
// times, not that every counter serialized identically.
//
// The lines are SORTED rather than kept in emission order, and that is not a
// tolerance knob: gosim emits on two channels that race with each other on
// capture. Go-side events (node_joined, node_left, metrics) go straight to the
// broadcast callback, while C-side events (message_delivered, anomaly,
// route_added) are fprintf'd to a pipe drained by a separate goroutine
// (runScenarioHeadless in bridge.go). The simulation itself is single-threaded
// over virtual time and fully deterministic, but the order in which those two
// streams interleave in the captured slice is not. Sorting removes exactly that
// capture artifact and nothing else: each line still carries its own virtual
// timestamp, so any genuine change in what happened, to which node, or when,
// still changes the fingerprint.
func fingerprintRun(run *scenarioRun) string {
	var lines []string
	for _, e := range run.events {
		typ, _ := e["type"].(string)
		switch typ {
		case "message_sent", "message_delivered", "message_dropped", "anomaly",
			"node_joined", "node_left", "route_added":
			lines = append(lines, fmt.Sprintf("%v|%v|%v|%v|%v|%v", typ, e["timestamp_us"],
				e["node"], e["packet_id"], e["anomaly_type"], e["details"]))
		}
	}
	sort.Strings(lines)
	return strings.Join(lines, "\n")
}
