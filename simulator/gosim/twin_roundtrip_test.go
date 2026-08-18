package main

// Mesh digital twin: round-trip verification (simulation).
//
// The strongest check available without a field deployment: run a scenario
// whose connectivity the simulator itself decides, export every node's observed
// state through the firmware's own bramble.exportTopology builder
// (main/topology_export.c, compiled into the sim), re-import those documents,
// and compare the reconstructed link graph against what the radio actually
// carried. Then run the RECONSTRUCTION and export from it again, so the whole
// path, export to import to scenario to export, is closed.
//
// What this verifies is the fidelity of the pipeline, not of the radio model:
// it says that a mesh's reported neighbour tables reconstruct the links that
// mesh really had, and that a twin built from them reproduces those same links
// exactly. It says nothing about how well the simulator's propagation matches a
// real hillside, which is why docs/digital-twin.md labels every twin number as
// simulation.

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"testing"
)

// twinRoundTripScenario is a four-node line at 100-unit spacing. At the default
// SF9/125 kHz PHY the derived range is about 150 units, so each node hears only
// its immediate neighbours: a topology with a known answer (three reciprocal
// links, six directed) and one that exercises multi-hop routing rather than an
// all-in-range clique.
func twinRoundTripScenario(durationMs int) string {
	type node struct {
		ID string  `json:"id"`
		X  float64 `json:"x"`
		Y  float64 `json:"y"`
	}
	var nodes []node
	for i := 0; i < 4; i++ {
		nodes = append(nodes, node{ID: fmt.Sprintf("N%d", i), X: float64(i * 100)})
	}
	sc := map[string]any{
		"name":        "twin-roundtrip-line",
		"mode":        "deterministic",
		"duration_ms": durationMs,
		"seed":        11,
		"nodes":       nodes,
		"radio":       map[string]any{"loss_pct": 0},
		"events": []any{
			map[string]any{"at_ms": 90000, "type": "send_message", "src": "N0", "dest": "N3"},
			map[string]any{"at_ms": 180000, "type": "send_message", "src": "N3", "dest": "N0"},
		},
	}
	b, err := json.Marshal(sc)
	if err != nil {
		panic(err)
	}
	return string(b)
}

// importExports parses a captured export set and merges it.
func importExports(t *testing.T, docs []twinObservedExport) *twinGraph {
	t.Helper()
	var parsed []*twinExport
	for _, d := range docs {
		exp, err := parseTwinExport(d.JSON, d.ScenarioID)
		if err != nil {
			t.Fatalf("parseTwinExport(%s): %v\n%s", d.ScenarioID, err, d.JSON)
		}
		parsed = append(parsed, exp)
	}
	g, err := buildTwinGraph(parsed)
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	return g
}

// scenarioIDByAddress maps each export's node address to the scenario id of the
// node that produced it. gosim derives a node's address from its scenario id
// (node_array_add), so the address changes when the same mesh is re-simulated
// under new ids; the id is what identifies a node across a re-import.
func scenarioIDByAddress(docs []twinObservedExport) map[string]string {
	out := map[string]string{}
	for _, d := range docs {
		var doc struct {
			Node struct {
				Address string `json:"address"`
			} `json:"node"`
		}
		if json.Unmarshal(d.JSON, &doc) == nil {
			out[doc.Node.Address] = d.ScenarioID
		}
	}
	return out
}

// linksByScenarioID renders a graph's links keyed by scenario id rather than
// address, with their link quality, so two runs of the same mesh are comparable
// even though each run derives its own addresses.
func linksByScenarioID(t *testing.T, g *twinGraph, idOf map[string]string) []string {
	t.Helper()
	var out []string
	for _, l := range g.Links {
		from, ok := idOf[l.From]
		if !ok {
			t.Fatalf("link from unknown address %s", l.From)
		}
		to, ok := idOf[l.To]
		if !ok {
			t.Fatalf("link to unknown address %s", l.To)
		}
		out = append(out, fmt.Sprintf("%s>%s@%d/%d", from, to, l.RSSI, l.SNR))
	}
	sort.Strings(out)
	return out
}

func runTwinScenarioFile(t *testing.T, body string) *scenarioRunResult {
	t.Helper()
	path := filepath.Join(t.TempDir(), "scenario.json")
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatalf("write scenario: %v", err)
	}
	result, err := runScenario(path)
	if err != nil {
		t.Fatalf("runScenario: %v", err)
	}
	return result
}

func TestTwinRoundTripReconstructsTheLinksTheRadioCarried(t *testing.T) {
	const durationMs = 400000
	run := runTwinScenarioFile(t, twinRoundTripScenario(durationMs))

	docs := run.TwinExports()
	if len(docs) != 4 {
		t.Fatalf("%d exports captured, want one per node", len(docs))
	}
	graph := importExports(t, docs)
	idOf := scenarioIDByAddress(docs)

	if len(graph.Nodes) != 4 {
		t.Fatalf("reconstructed %d nodes, want 4", len(graph.Nodes))
	}
	if len(graph.UnexportedNodes()) != 0 {
		t.Fatalf("every node exported, yet %v are one-sided", graph.UnexportedNodes())
	}
	if len(graph.UnobservedLinks()) != 0 {
		t.Fatalf("every node exported, yet %d link directions were assumed",
			len(graph.UnobservedLinks()))
	}

	// Ground truth: the ordered pairs the radio model itself says can carry a
	// frame. The reconstruction has to be exactly that set, no link invented
	// and none dropped.
	var want []string
	for pair := range run.AudibleLinks() {
		want = append(want, pair[0]+">"+pair[1])
	}
	sort.Strings(want)

	var got []string
	for _, l := range graph.Links {
		got = append(got, idOf[l.From]+">"+idOf[l.To])
	}
	sort.Strings(got)

	if !equalStrings(got, want) {
		t.Fatalf("reconstructed links\n got %v\nwant %v", got, want)
	}
	// The line topology's own answer, so a change in the radio model that
	// silently made every node hear every other would not slip through by
	// agreeing with itself.
	if len(want) != 6 {
		t.Fatalf("the four-node line carried %d directed links, want 6: %v", len(want), want)
	}
}

func TestTwinRoundTripReExportsTheSameGraph(t *testing.T) {
	const durationMs = 400000
	first := runTwinScenarioFile(t, twinRoundTripScenario(durationMs))
	firstDocs := first.TwinExports()
	firstGraph := importExports(t, firstDocs)
	firstIDs := scenarioIDByAddress(firstDocs)

	// Re-simulate the RECONSTRUCTION: a link-mode scenario whose node ids are
	// the first run's addresses, then export from it exactly as before.
	sc := buildTwinScenario(firstGraph, "twin-roundtrip", 11, durationMs, nil)
	body, err := sc.JSON()
	if err != nil {
		t.Fatalf("scenario JSON: %v", err)
	}
	second := runTwinScenarioFile(t, string(body))
	secondDocs := second.TwinExports()
	secondGraph := importExports(t, secondDocs)

	// A node of the second run carries the first run's address as its scenario
	// id, so composing the two maps lands both graphs in first-run id space.
	secondIDs := map[string]string{}
	for addr, firstAddr := range scenarioIDByAddress(secondDocs) {
		id, ok := firstIDs[firstAddr]
		if !ok {
			t.Fatalf("re-run node %s does not correspond to any first-run node", firstAddr)
		}
		secondIDs[addr] = id
	}

	want := linksByScenarioID(t, firstGraph, firstIDs)
	got := linksByScenarioID(t, secondGraph, secondIDs)
	if !equalStrings(got, want) {
		t.Fatalf("re-export changed the link graph\n got %v\nwant %v", got, want)
	}
	if len(secondGraph.UnobservedLinks()) != 0 {
		t.Fatalf("the twin re-run left %d link directions unobserved",
			len(secondGraph.UnobservedLinks()))
	}
}

func TestTwinExportedDocumentsMatchTheRPCSchema(t *testing.T) {
	run := runTwinScenarioFile(t, twinRoundTripScenario(200000))
	docs := run.TwinExports()
	if len(docs) == 0 {
		t.Fatal("no exports captured")
	}

	// Every key api/openapi.yaml marks required has to be present, since the
	// documents these tests feed the importer are the same shape a device
	// returns and the fixtures under testdata/twin claim to be.
	var doc map[string]any
	if err := json.Unmarshal(docs[0].JSON, &doc); err != nil {
		t.Fatalf("export is not JSON: %v", err)
	}
	for _, key := range []string{"twin_schema", "node", "radio", "neighbors", "routes"} {
		if _, ok := doc[key]; !ok {
			t.Fatalf("export has no %q: %s", key, docs[0].JSON)
		}
	}
	if doc["twin_schema"].(float64) != float64(twinSchemaSupported) {
		t.Fatalf("export schema %v, importer reads %d", doc["twin_schema"], twinSchemaSupported)
	}
	node := doc["node"].(map[string]any)
	for _, key := range []string{"address", "firmware_version", "protocol_version", "hardware",
		"uptime_s"} {
		if _, ok := node[key]; !ok {
			t.Fatalf("export node has no %q: %s", key, docs[0].JSON)
		}
	}
	radio := doc["radio"].(map[string]any)
	for _, key := range []string{"frequency_mhz", "sf", "bw_hz", "coding_rate", "tx_power_dbm",
		"region", "regulatory", "max_duty_cycle_pct", "duty_cycle_enforced"} {
		if _, ok := radio[key]; !ok {
			t.Fatalf("export radio has no %q: %s", key, docs[0].JSON)
		}
	}
	// The PHY the scenario ran at, which is what prices the twin's airtime.
	if radio["sf"].(float64) != 9 || radio["bw_hz"].(float64) != 125000 {
		t.Fatalf("export PHY %v / %v, want the plan default SF9 / 125 kHz",
			radio["sf"], radio["bw_hz"])
	}
}

// twinDocKeys lists an object's keys, dropping the ones the schema marks
// optional, so two documents are comparable whether or not a node or a
// neighbour happened to have a name.
func twinDocKeys(t *testing.T, obj any, where string) []string {
	t.Helper()
	m, ok := obj.(map[string]any)
	if !ok {
		t.Fatalf("%s is not an object", where)
	}
	var out []string
	for k := range m {
		if k == "name" {
			continue
		}
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// firstDocWithRoutes returns a decoded export that carries at least one route,
// so the route object's shape can be compared.
func firstDocWithRoutes(t *testing.T, docs []twinObservedExport) map[string]any {
	t.Helper()
	for _, d := range docs {
		var m map[string]any
		if json.Unmarshal(d.JSON, &m) != nil {
			continue
		}
		if routes, ok := m["routes"].([]any); ok && len(routes) > 0 {
			if nb, ok := m["neighbors"].([]any); ok && len(nb) > 0 {
				return m
			}
		}
	}
	t.Fatal("no captured export carried both a neighbour and a route")
	return nil
}

// The committed fixtures are what docs/digital-twin.md works through and what
// the importer tests read, so they have to be the shape a device really
// returns rather than a plausible imitation of it. Comparing them field for
// field against a document the firmware's own builder wrote is what keeps them
// honest: a schema change that misses the fixtures fails here.
func TestTwinFixturesMatchAFirmwareWrittenExport(t *testing.T) {
	run := runTwinScenarioFile(t, twinRoundTripScenario(400000))
	real := firstDocWithRoutes(t, run.TwinExports())

	for _, name := range []string{"basecamp", "creek", "ridge", "tower"} {
		t.Run(name, func(t *testing.T) {
			data, err := os.ReadFile(filepath.Join("testdata", "twin", name+".json"))
			if err != nil {
				t.Fatalf("read fixture: %v", err)
			}
			var fixture map[string]any
			if err := json.Unmarshal(data, &fixture); err != nil {
				t.Fatalf("fixture is not JSON: %v", err)
			}

			cases := []struct {
				where string
				got   any
				want  any
			}{
				{"document", fixture, real},
				{"node", fixture["node"], real["node"]},
				{"radio", fixture["radio"], real["radio"]},
				{"neighbor", fixture["neighbors"].([]any)[0], real["neighbors"].([]any)[0]},
				{"route", fixture["routes"].([]any)[0], real["routes"].([]any)[0]},
			}
			for _, c := range cases {
				got := twinDocKeys(t, c.got, "fixture "+c.where)
				want := twinDocKeys(t, c.want, "exported "+c.where)
				if !equalStrings(got, want) {
					t.Fatalf("%s keys\n got %v\nwant %v", c.where, got, want)
				}
			}
		})
	}
}

// TestTwinExportsCarryTheLinkQualityTheRadioComputed pins the SNR half of the
// round trip. The neighbour table is what an export reports, and it is filled
// from the rssi and snr the radio computed for each reception, so an export
// whose SNR column were a constant would make every link-quality comparison in
// this file vacuous: two all-zero vectors agree with each other and detect
// nothing.
//
// The simulator's position mode derives SNR as RSSI minus a -120 dBm noise
// floor, with up to 2 dB of jitter either way (radio_deliver_packet in
// simulator/engine/sim_radio.c), which is the range checked here.
func TestTwinExportsCarryTheLinkQualityTheRadioComputed(t *testing.T) {
	const noiseFloorDBm = -120
	const jitterDB = 2

	run := runTwinScenarioFile(t, twinRoundTripScenario(400000))
	docs := run.TwinExports()
	if len(docs) != 4 {
		t.Fatalf("%d exports captured, want one per node", len(docs))
	}

	entries := 0
	for _, d := range docs {
		var doc struct {
			Neighbors []struct {
				Address string `json:"address"`
				RSSI    int    `json:"rssi"`
				SNR     int    `json:"snr"`
			} `json:"neighbors"`
		}
		if err := json.Unmarshal(d.JSON, &doc); err != nil {
			t.Fatalf("export from %s is not JSON: %v", d.ScenarioID, err)
		}
		for _, nb := range doc.Neighbors {
			entries++
			want := nb.RSSI - noiseFloorDBm
			if nb.SNR < want-jitterDB || nb.SNR > want+jitterDB {
				t.Fatalf("%s heard %s at %d dBm and reports %d dB SNR, want %d +/- %d",
					d.ScenarioID, nb.Address, nb.RSSI, nb.SNR, want, jitterDB)
			}
		}
	}
	if entries != 6 {
		t.Fatalf("%d neighbour entries across the line, want 6", entries)
	}
}
