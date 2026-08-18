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
// runScenario entry point the rest of the package uses, and asserts the
// invariant each scenario exists to check. gosim's own CI job already runs
// `go test -count=1 ./...`, so these gate on every PR with no workflow change.
//
// Determinism: every scenario gated here declares "mode": "deterministic" and
// runs on virtual time with a fixed RNG seed and identity keys derived from the
// node id, so a run is reproducible bit for bit. There are deliberately no
// retries, no sleeps, and no tolerance windows: per CLAUDE.md, a scenario that
// only passes sometimes is a bug to fix, not a knob to tune.
//
// Issue #144 closed the gaps this header used to list: node_join now parses
// coordinates and a coordinate-less rejoin restores the node's original
// scenario position; a rejoin reuses the existing node entry (identity kept,
// volatile state cleared) instead of appending a duplicate; mesh_partition
// carries its detection time; the route_loop detector checks at each relay's
// FORWARD (keyed on arriving hop_limit) so flood rebroadcasts and ACK
// retransmissions no longer false-positive; and the reliability machinery
// (retransmit ladder, duplicate re-ACK, RERR failfast) actually runs. All
// three anomaly scenarios (partition incl. heal, black-hole, route-loop) are
// gated below, non-vacuously: each proves real deliveries around its failure
// mode before asserting the failure mode itself.

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
	events []map[string]any
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
	result, err := runScenario(path)
	if err != nil {
		t.Fatalf("runScenario(%s): %v", path, err)
	}
	run := &scenarioRun{
		addrOf: map[string]string{},
		idOf:   map[string]string{},
	}
	for _, line := range result.Lines() {
		var evt map[string]any
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
func (r *scenarioRun) anomalies(anomalyType string) []map[string]any {
	var out []map[string]any
	for _, e := range r.events {
		if e["type"] == "anomaly" && e["anomaly_type"] == anomalyType {
			out = append(out, e)
		}
	}
	return out
}

// finalMetrics returns the single terminal final_metrics event.
func (r *scenarioRun) finalMetrics(t *testing.T) map[string]any {
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
		raw, ok := e["path"].([]any)
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
// 100-unit spacing, so C is the sole bridge between {A,B} and {D,E}. All
// three phases are gated here; the heal phase joined the gate when issue
// #144 fixed node_join (rejoin reuses the entry and restores position).
//
// Phase 1 (connectivity): before anything is killed, A->E and E->A must each
// traverse the full 4-hop line and have their delivery receipt return to the
// originator over the exact reverse path. This is the baseline the partition
// phase is only meaningful against: without it, "nothing was delivered after
// the partition" would prove nothing.
//
// Phase 2 (partition): killing C must partition the mesh, be detected as such
// (naming exactly the nodes that became unreachable, at the kill's virtual
// time, not 0), and every message sent while the mesh is split must fail to
// reach the far side.
//
// Phase 3 (heal): C's scripted rejoin at its original coordinates rebuilds
// the bridge, but a reboot loses C's routes while its neighbors keep
// stale-ACTIVE routes through it. The first post-heal send (E->A at 185s)
// therefore dies at C, whose no-route RERR (broken_next_hop = C itself,
// firmware's forward_data_packet semantics) propagates one ring and
// fast-fails E's pending ack: the send must be dropped with reason
// route_broken, not delivered. That transit teaches every relay a route
// toward E, so the next send (A->E at 215s) must deliver AND confirm over
// the exact line path, and its own transit re-teaches the A direction so
// the final send (E->A at 240s) must too. This is the AODV cost of a
// bridge reboot, reproduced end to end.
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
	eToA := sentByE[0] // the 35s E->A send

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
	// Issue #144: mesh_partition used to be the only anomaly emitted with
	// timestamp_us 0 instead of its detection time. The kill is scripted at
	// 60s and the sweep runs from the node_left handler, so the detection
	// time IS the kill time.
	if ts, _ := parts[0]["timestamp_us"].(float64); ts != 60000000 {
		t.Errorf("mesh_partition timestamp_us = %v, want 60000000 (the kill's virtual time; "+
			"0 means the detection-time regression is back)", parts[0]["timestamp_us"])
	}

	// Every message the scenario sends while C is down (three of them, at 70s,
	// 80s and 90s) must fail to cross: a partitioned mesh that still delivers
	// would mean the radio range model is not being enforced.
	if len(sentByA) != 4 || len(sentByE) != 4 {
		t.Fatalf("sends by A = %d, by E = %d, want 4 each (pre, partitioned, and heal phases); "+
			"the scenario's event script and this gate have drifted apart",
			len(sentByA), len(sentByE))
	}
	partitionedSends := []string{sentByA[1], sentByA[2], sentByE[1]}
	for _, pid := range partitionedSends {
		if at := run.deliveredAt(pid); len(at) > 0 {
			t.Errorf("packet %s was delivered at %v despite the mesh being partitioned by C's "+
				"death; nothing may cross a partition", pid, at)
		}
	}

	// --- Phase 3: heal. See the doc comment for why the first post-heal ---
	// --- send must die and the two after it must confirm.               ---
	healTeach := sentByE[2] // 185s E->A: dies at rebooted C, fast-failed
	if at := run.deliveredAt(healTeach); len(at) > 0 {
		t.Errorf("packet %s (first post-heal send) was delivered at %v; it must die at the "+
			"rebooted bridge C, which has no routes yet", healTeach, at)
	}
	teachDropped := false
	for _, ev := range run.events {
		if ev["type"] == "message_dropped" && ev["packet_id"] == healTeach {
			if reason, _ := ev["reason"].(string); reason == "route_broken" {
				teachDropped = true
			} else {
				t.Errorf("packet %s dropped with reason %q, want route_broken (C's no-route "+
					"RERR must fast-fail the pending ack, not let it exhaust retries)",
					healTeach, ev["reason"])
			}
		}
	}
	if !teachDropped {
		t.Errorf("packet %s (first post-heal send) was never dropped with reason route_broken; "+
			"the RERR teardown chain from the rebooted bridge is not reaching the sender",
			healTeach)
	}

	for _, tc := range []struct {
		label    string
		packetID string
		from     string
		wantPath []string
	}{
		{"A->E (post-heal)", sentByA[3], e, []string{"B", "C", "D"}},
		{"E->A (post-heal)", sentByE[3], a, []string{"D", "C", "B"}},
	} {
		rec, ok := run.findReceipt(tc.packetID)
		if !ok {
			t.Errorf("%s (packet %s): no delivery receipt returned to the originator; the "+
				"healed line must confirm end to end through the rejoined C or the heal is "+
				"cosmetic", tc.label, tc.packetID)
			continue
		}
		if rec.from != tc.from {
			t.Errorf("%s: receipt from = %s, want %s", tc.label, rec.from, tc.from)
		}
		if strings.Join(rec.path, ",") != strings.Join(tc.wantPath, ",") {
			t.Errorf("%s: receipt path = %v, want %v (the only route across the healed line, "+
				"through the rejoined C)", tc.label, rec.path, tc.wantPath)
		}
	}

	// The scenario's terminal accounting must agree: the two pre-partition
	// and two confirming post-heal messages, nothing else.
	fm := run.finalMetrics(t)
	if confirmed, _ := fm["confirmed"].(float64); confirmed != 4 {
		t.Errorf("confirmed = %v, want 4 (two pre-partition and two post-heal)",
			fm["confirmed"])
	}
	if delivered, _ := fm["delivered"].(float64); delivered != 4 {
		t.Errorf("delivered = %v, want 4 (two pre-partition and two post-heal)",
			fm["delivered"])
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
//
// The scenario's movement phase (move_node of C out of the line at 45s, back
// at 70s) does execute: move_node is the engine's spelling and every gated
// scenario uses it. While C is out the line is split, so sends in that window
// return no receipt and simply do not appear below; every receipt that DOES
// return is still asserted to be a correct line-walk. The assertion is
// topology-agnostic by construction (it derives the expected path from each
// receipt's own endpoints), so it holds across the partition and heal without
// re-derivation.
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

// TestScenarioReliabilityAckRetry gates
// simulator/scenarios/reliability-ack-retry.json, the scenario that exists to
// prove the ACK retransmit ladder actually recovers lost frames on a lossy
// multi-hop line.
//
// The topology is a 3-hop line A-B-C-D at 100-unit spacing (150-unit range, so
// each node reaches only its neighbours) carrying six well-spaced bidirectional
// A<->D sends under 5 percent intermittent packet loss. When #212 rebuilt this
// fixture it had to leave it ungated: the retransmit ladder was inert because
// the pending-ack state machine was double-driven, a bug coupled to the
// route_loop-detector false positive tracked in #144. #240 fixed both, so the
// ladder is now live and this scenario can finally assert what it was built for.
//
// Three invariants, each of which is exactly what #240 unblocked:
//   - every one of the six messages is delivered end to end; 5 percent loss on
//     three hops must be fully masked by retransmission, not merely survived on
//     average.
//   - the ladder is deliberate, not incidental: at least one message is
//     delivered ON a retry (delivered_on_retry > 0), so a regression that
//     silently re-inerts the pending-ack machine, delivering only the frames
//     that happened not to drop, fails here even if the average rate still looks
//     healthy.
//   - every delivery receipt that returns walks the exact line path between its
//     endpoints, so a retry storm cannot paper over a routing detour.
func TestScenarioReliabilityAckRetry(t *testing.T) {
	run := runGatedScenario(t, "reliability-ack-retry")

	fm := run.finalMetrics(t)
	metric := func(key string) float64 {
		v, ok := fm[key].(float64)
		if !ok {
			t.Fatalf("final_metrics missing numeric %q: got %v", key, fm[key])
		}
		return v
	}

	// The scenario scripts exactly six sends; every one must be delivered.
	const wantSends = 6
	if got := metric("delivered"); got != wantSends {
		t.Errorf("delivered = %v, want %d; this is a reliability scenario, so 5%% loss on a "+
			"3-hop line must be fully masked by the retransmit ladder, not partially delivered",
			got, wantSends)
	}

	// The ladder must actually fire and recover a loss, or the scenario is inert,
	// which is exactly the state #212 left it in pending the #240 fix.
	if got := metric("retried"); got <= 0 {
		t.Errorf("retried = %v, want > 0; with 5%% loss the ACK ladder must retransmit, and a "+
			"run with zero retries means the pending-ack machine went inert again", got)
	}
	if got := metric("delivered_on_retry"); got <= 0 {
		t.Errorf("delivered_on_retry = %v, want > 0; at least one message must reach its "+
			"destination on a retransmit, proving the ladder recovers real losses rather than "+
			"the run happening to drop nothing", got)
	}

	// Every returned receipt must have walked the direct line path between its
	// endpoints: a retry storm must not mask a routing detour.
	line := []string{"A", "B", "C", "D"}
	indexOf := map[string]int{}
	for i, id := range line {
		indexOf[id] = i
	}
	recs := run.receipts()
	if len(recs) == 0 {
		t.Fatal("no delivery receipt returned to any originator; a reliable-delivery scenario " +
			"with no returned receipt is inert rather than passing")
	}
	multiHop := 0
	for _, rec := range recs {
		srcID, sok := indexOf[rec.to]
		dstID, dok := indexOf[run.idOf[rec.from]]
		if !sok || !dok {
			t.Errorf("receipt %s references nodes outside the line A-B-C-D: to=%s from=%s",
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
			t.Errorf("receipt %s (%s -> %s): return path = %v, want %v; on a line the only correct "+
				"route is the direct walk, so a differing path means routing detoured or misreported it",
				rec.packetID, rec.to, run.idOf[rec.from], rec.path, want)
		}
		if rec.hops != len(rec.path) {
			t.Errorf("receipt %s: hops = %d but path lists %d intermediate nodes (%v)",
				rec.packetID, rec.hops, len(rec.path), rec.path)
		}
		if rec.hops >= 2 {
			multiHop++
		}
	}
	if multiHop == 0 {
		t.Errorf("no receipt traversed two or more intermediate hops (%d receipts); the A<->D "+
			"exchanges span the full 3-hop line, so a run without a multi-hop receipt is not "+
			"exercising the path this scenario exists to prove", len(recs))
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
	for _, name := range []string{"anomaly-partition", "reliability-path-trace", "reliability-ack-retry"} {
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
// (runScenario in bridge.go). The simulation itself is single-threaded
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

// TestScenarioLocationSharing gates simulator/scenarios/location-sharing.json
// (issue #172): every scripted send_location must originate a real
// PKT_TYPE_LOCATION broadcast, and every other node in this single-hop square
// must receive and cache it with the exact coordinates the scenario supplied.
// Before the fix the engine had no implementation for send_location at all, so
// the scenario ran only its two chat messages while claiming to verify
// position updates. Simulation result: deterministic under the scenario seed.
func TestScenarioLocationSharing(t *testing.T) {
	run := runGatedScenario(t, "location-sharing")

	type coord struct {
		lat, lon float64
	}
	// One entry per scripted send_location, in scenario order.
	sends := []struct {
		node string
		c    coord
	}{
		{"A", coord{370049000, -1235194000}},
		{"B", coord{370051000, -1235180000}},
		{"A", coord{370053000, -1235190000}},
		{"C", coord{370045000, -1235175000}},
		{"D", coord{370040000, -1235200000}},
		{"A", coord{370056000, -1235185000}},
		{"B", coord{370060000, -1235170000}},
		{"A", coord{370058000, -1235183000}},
	}

	var sent []map[string]any
	received := map[coord]map[string]bool{} // coord -> set of receiving node ids
	for _, e := range run.events {
		switch e["type"] {
		case "location_sent":
			sent = append(sent, e)
		case "location_received":
			lat, _ := e["lat_e7"].(float64)
			lon, _ := e["lon_e7"].(float64)
			node, _ := e["node"].(string)
			c := coord{lat, lon}
			if received[c] == nil {
				received[c] = map[string]bool{}
			}
			received[c][node] = true
		}
	}

	if len(sent) != len(sends) {
		t.Fatalf("location_sent count = %d, want %d (one per scripted send_location)",
			len(sent), len(sends))
	}
	totalReceived := 0
	receivedFrom := map[string]map[string]bool{} // sender node -> receiver set
	for i, want := range sends {
		node, _ := sent[i]["node"].(string)
		lat, _ := sent[i]["lat_e7"].(float64)
		lon, _ := sent[i]["lon_e7"].(float64)
		if node != want.node || lat != want.c.lat || lon != want.c.lon {
			t.Errorf("location_sent[%d] = node %s (%v, %v), want node %s (%v, %v)",
				i, node, lat, lon, want.node, want.c.lat, want.c.lon)
		}
		got := received[want.c]
		if got[want.node] {
			t.Errorf("update %d from %s was received by its own sender", i, want.node)
		}
		totalReceived += len(got)
		if receivedFrom[want.node] == nil {
			receivedFrom[want.node] = map[string]bool{}
		}
		for r := range got {
			receivedFrom[want.node][r] = true
		}
	}
	// The scenario's stated purpose: all contacts receive timely location
	// updates. Position broadcasts are fire-and-forget (real-time presence,
	// firmware never retransmits them), so under the scenario's 2 percent
	// loss plus chat-message collisions a single copy may legitimately die;
	// asserting every update reaches all 3 peers would gate collision-
	// pattern trivia and break on unrelated timing changes. What must hold:
	// every node's position reached EVERY other node at least once across
	// its updates (pair coverage, the presence guarantee), coordinates are
	// bit-exact when received (asserted via the coord-keyed map above), and
	// the overall reception rate stays high.
	for _, sender := range []string{"A", "B", "C", "D"} {
		for _, receiver := range []string{"A", "B", "C", "D"} {
			if sender == receiver {
				continue
			}
			if !receivedFrom[sender][receiver] {
				t.Errorf("%s never received any location update from %s; the scenario "+
					"exists to verify all contacts get position updates", receiver, sender)
			}
		}
	}
	if maxPossible := 3 * len(sends); totalReceived < maxPossible-3 {
		t.Errorf("location receptions = %d of %d possible, want at least %d; more than an "+
			"occasional lost copy means the broadcast path regressed",
			totalReceived, maxPossible, maxPossible-3)
	}

	// The two chat messages that always worked must keep delivering, so the
	// location traffic did not crowd them out.
	fm := run.finalMetrics(t)
	if delivered, _ := fm["delivered"].(float64); delivered != 2 {
		t.Errorf("delivered = %v, want 2 (both scripted chat messages)", fm["delivered"])
	}

	assertNoRoutingPathologies(t, run)
}

// TestScenarioAnomalyBlackHole gates simulator/scenarios/anomaly-black-hole.json
// (issue #144).
//
// The 5-node line confirms a baseline exchange in both directions, then C is
// killed and rejoined WITHOUT coordinates: the rejoin must restore its
// original (200,0) position, and models a reboot: C's route table is gone
// while its neighbors keep stale-ACTIVE routes through it. Six rapid A->E
// sends then pour DATA into the rebooted C, which receives but cannot
// forward: that must fire the black_hole anomaly on C, and every one of
// those sends must be fast-failed by C's no-route RERR chain (reason
// route_broken), not delivered and not left to rot in retry limbo.
func TestScenarioAnomalyBlackHole(t *testing.T) {
	run := runGatedScenario(t, "anomaly-black-hole")

	a := run.addr(t, "A")
	e := run.addr(t, "E")

	sentByA := run.messagesSentBy("A")
	sentByE := run.messagesSentBy("E")
	if len(sentByA) != 7 || len(sentByE) != 1 {
		t.Fatalf("sends by A = %d, by E = %d, want 7 and 1; the scenario's event script and "+
			"this gate have drifted apart", len(sentByA), len(sentByE))
	}

	// Baseline: both directions confirmed over the exact line path.
	for _, tc := range []struct {
		label    string
		packetID string
		from     string
		wantPath []string
	}{
		{"A->E (baseline)", sentByA[0], e, []string{"B", "C", "D"}},
		{"E->A (baseline)", sentByE[0], a, []string{"D", "C", "B"}},
	} {
		rec, ok := run.findReceipt(tc.packetID)
		if !ok {
			t.Errorf("%s (packet %s): no delivery receipt returned; without a confirmed "+
				"baseline the black-hole phase proves nothing", tc.label, tc.packetID)
			continue
		}
		if rec.from != tc.from {
			t.Errorf("%s: receipt from = %s, want %s", tc.label, rec.from, tc.from)
		}
		if strings.Join(rec.path, ",") != strings.Join(tc.wantPath, ",") {
			t.Errorf("%s: receipt path = %v, want %v", tc.label, rec.path, tc.wantPath)
		}
	}

	// The coordinate-less rejoin must restore C's original position: at
	// (0,0) it would no longer bridge the line and the black-hole phase
	// would silently test a partition instead.
	rejoined := false
	for _, ev := range run.events {
		if ev["type"] == "node_joined" && ev["node"] == "C" {
			if ts, _ := ev["timestamp_us"].(float64); ts > 0 {
				rejoined = true
				x, _ := ev["x"].(float64)
				y, _ := ev["y"].(float64)
				if x != 200 || y != 0 {
					t.Errorf("C rejoined at (%v,%v), want its original (200,0); a "+
						"coordinate-less node_join must restore the scenario position", x, y)
				}
			}
		}
	}
	if !rejoined {
		t.Fatal("C never rejoined; the black-hole phase cannot have run")
	}

	// The namesake anomaly: C receives the rapid sends and forwards none.
	holes := run.anomalies("black_hole")
	if len(holes) != 1 {
		t.Fatalf("black_hole anomalies = %d, want exactly 1; got %v", len(holes), holes)
	}
	if node, _ := holes[0]["node"].(string); node != "C" {
		t.Errorf("black_hole fired on %q, want C (the rebooted relay is the hole)", node)
	}

	// Every rapid send dies fast with route_broken; none is delivered.
	for _, pid := range sentByA[1:] {
		if at := run.deliveredAt(pid); len(at) > 0 {
			t.Errorf("packet %s was delivered at %v despite the rebooted C having no routes",
				pid, at)
		}
		dropped := false
		for _, ev := range run.events {
			if ev["type"] == "message_dropped" && ev["packet_id"] == pid &&
				ev["reason"] == "route_broken" {
				dropped = true
			}
		}
		if !dropped {
			t.Errorf("packet %s was never fast-failed with route_broken; the RERR chain from "+
				"the black hole is not reaching the sender", pid)
		}
	}

	fm := run.finalMetrics(t)
	if delivered, _ := fm["delivered"].(float64); delivered != 2 {
		t.Errorf("delivered = %v, want 2 (the baseline only)", fm["delivered"])
	}
	if confirmed, _ := fm["confirmed"].(float64); confirmed != 2 {
		t.Errorf("confirmed = %v, want 2 (the baseline only)", fm["confirmed"])
	}

	// The loop detector must stay quiet: a black hole swallows packets, it
	// does not loop them.
	for _, an := range run.anomalies("route_loop") {
		t.Errorf("route_loop anomaly fired: node=%v details=%v", an["node"], an["details"])
	}
}

// TestScenarioAnomalyRouteLoop gates simulator/scenarios/anomaly-route-loop.json
// (issue #144).
//
// The cross topology (E reachable only through C) confirms four baseline
// exchanges, then churns C through two kill/rejoin cycles with traffic
// before, during, and after. Each cycle plays the AODV reconvergence
// pattern the anomaly-partition gate documents: the first send into the
// rebooted C dies on its stale route (route_broken), a second send in the
// same direction burns down the upstream stale route, and rediscovery then
// confirms end to end. The headline assertion is ZERO route_loop anomalies
// across all of it: with the detector now checking forwards (arriving
// hop_limit as the discriminator), a quiet detector over a run with real
// confirmed deliveries validates the routing design instead of passing
// vacuously over a dead mesh, which is exactly the trap this scenario used
// to be (issue #144: zero DATA ever originated).
func TestScenarioAnomalyRouteLoop(t *testing.T) {
	run := runGatedScenario(t, "anomaly-route-loop")

	sentByA := run.messagesSentBy("A")
	sentByB := run.messagesSentBy("B")
	sentByD := run.messagesSentBy("D")
	sentByE := run.messagesSentBy("E")
	if len(sentByA) != 4 || len(sentByB) != 1 || len(sentByD) != 1 || len(sentByE) != 3 {
		t.Fatalf("sends A=%d B=%d D=%d E=%d, want 4/1/1/3; the scenario's event script and "+
			"this gate have drifted apart", len(sentByA), len(sentByB), len(sentByD), len(sentByE))
	}

	// Baseline: all four exchanges confirm.
	for _, tc := range []struct {
		label    string
		packetID string
	}{
		{"A->E (baseline)", sentByA[0]},
		{"E->A (baseline)", sentByE[0]},
		{"B->D (baseline)", sentByB[0]},
		{"D->B (baseline)", sentByD[0]},
	} {
		if _, ok := run.findReceipt(tc.packetID); !ok {
			t.Errorf("%s (packet %s): no delivery receipt returned; the baseline must confirm "+
				"before the churn phases mean anything", tc.label, tc.packetID)
		}
	}

	// Churn cycles: teach/teardown sends die fast, rediscoveries confirm.
	for _, pid := range []string{sentByA[1], sentByA[2], sentByE[1]} {
		if at := run.deliveredAt(pid); len(at) > 0 {
			t.Errorf("packet %s was delivered at %v; it must die on the stale route through "+
				"the rebooted C", pid, at)
		}
		dropped := false
		for _, ev := range run.events {
			if ev["type"] == "message_dropped" && ev["packet_id"] == pid &&
				ev["reason"] == "route_broken" {
				dropped = true
			}
		}
		if !dropped {
			t.Errorf("packet %s was never fast-failed with route_broken", pid)
		}
	}
	for _, tc := range []struct {
		label    string
		packetID string
	}{
		{"A->E (post-churn rediscovery)", sentByA[3]},
		{"E->A (post-churn rediscovery)", sentByE[2]},
	} {
		if _, ok := run.findReceipt(tc.packetID); !ok {
			t.Errorf("%s (packet %s): no delivery receipt returned; rediscovery through the "+
				"rebooted C must reconverge or the churn phases only ever proved failure",
				tc.label, tc.packetID)
		}
	}

	// The headline: no loops, ever, and non-vacuously so.
	for _, an := range run.anomalies("route_loop") {
		t.Errorf("route_loop anomaly fired: node=%v details=%v at t=%vus",
			an["node"], an["details"], an["timestamp_us"])
	}
	fm := run.finalMetrics(t)
	if confirmed, _ := fm["confirmed"].(float64); confirmed != 6 {
		t.Errorf("confirmed = %v, want 6 (4 baseline + 2 rediscoveries); a quiet loop "+
			"detector only means something over a mesh that actually delivered",
			fm["confirmed"])
	}
	// Killing C strands E both times (C is E's only link).
	if parts := run.anomalies("mesh_partition"); len(parts) != 2 {
		t.Errorf("mesh_partition anomalies = %d, want 2 (one per kill of E's only link)",
			len(parts))
	}
}
