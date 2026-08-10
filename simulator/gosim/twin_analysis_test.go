package main

// Mesh digital twin: the two analyses.
//
// Criticality is checked against hand-computable topologies, so a wrong answer
// is obvious rather than plausible. The capacity probe is checked for the
// things that must hold whatever the mesh does (one run per rate, the offered
// traffic actually scripted, the terminal message states summing to what was
// offered) plus a unit check of the knee arithmetic on a synthetic ramp, which
// is where the interesting logic is.

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// twinGraphFromDocs builds the graph the committed fixtures describe: a hub
// (tower) with three spokes, one of which (ridge) carries a fourth node
// (north-cabin) behind it.
func twinGraphFromDocs(t *testing.T) *twinGraph {
	t.Helper()
	g, err := buildTwinGraph([]*twinExport{
		twinFixture(t, "basecamp"), twinFixture(t, "creek"),
		twinFixture(t, "ridge"), twinFixture(t, "tower"),
	})
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	return g
}

// writeTwinScenarioFile renders a graph as a scenario in a temp dir.
func writeTwinScenarioFile(t *testing.T, g *twinGraph, durationMs int64,
	events []twinScenarioEvent) string {
	t.Helper()
	sc := buildTwinScenario(g, "twin-under-test", 1, durationMs, events)
	data, err := sc.JSON()
	if err != nil {
		t.Fatalf("scenario JSON: %v", err)
	}
	path := filepath.Join(t.TempDir(), "twin.json")
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatalf("write scenario: %v", err)
	}
	return path
}

// criticalityRow finds one node's row in the sweep.
func criticalityRow(t *testing.T, c *twinConnectivity, addr string) twinNodeCriticality {
	t.Helper()
	for _, r := range c.Nodes {
		if r.Address == addr {
			return r
		}
	}
	t.Fatalf("no criticality row for %s", addr)
	return twinNodeCriticality{}
}

func TestTwinCriticalityFindsTheCutNodes(t *testing.T) {
	g := twinGraphFromDocs(t)
	conn, err := twinAnalyzeConnectivity(writeTwinScenarioFile(t, g, 120000, nil), g)
	if err != nil {
		t.Fatalf("twinAnalyzeConnectivity: %v", err)
	}

	if len(conn.BaselineComponents) != 1 || len(conn.BaselineComponents[0]) != 5 {
		t.Fatalf("baseline components %v, want one piece of five nodes", conn.BaselineComponents)
	}
	if len(conn.Nodes) != 5 {
		t.Fatalf("%d criticality rows, want 5", len(conn.Nodes))
	}

	// The hub: losing it leaves ridge + north-cabin as the largest piece and
	// strands basecamp and creek on their own.
	tower := criticalityRow(t, conn, "3D4E5F60")
	if tower.Components != 3 {
		t.Fatalf("removing the hub leaves %d pieces, want 3", tower.Components)
	}
	if !equalStrings(tower.Isolated, []string{"0A1B2C3D", "2C3D4E5F"}) {
		t.Fatalf("hub removal strands %v", tower.Isolated)
	}
	if tower.Degree != 3 {
		t.Fatalf("hub degree %d, want 3", tower.Degree)
	}

	// The one relay: losing it strands only the node behind it.
	ridge := criticalityRow(t, conn, "1B2C3D4E")
	if !equalStrings(ridge.Isolated, []string{"4E5F6071"}) {
		t.Fatalf("relay removal strands %v, want just the node behind it", ridge.Isolated)
	}

	// Every leaf costs reach but not connectivity.
	for _, leaf := range []string{"0A1B2C3D", "2C3D4E5F", "4E5F6071"} {
		row := criticalityRow(t, conn, leaf)
		if len(row.Isolated) != 0 || row.Components != 1 {
			t.Fatalf("leaf %s removal: %d pieces, strands %v", leaf, row.Components, row.Isolated)
		}
	}
}

func TestTwinCriticalityReportsAnAlreadyPartitionedImport(t *testing.T) {
	// Two pairs that hear each other and nothing else: an import that is
	// already in two pieces, which every criticality row has to be read
	// against rather than reported as a fresh break.
	pairs := [][2]string{{"0A1B2C3D", "1B2C3D4E"}, {"2C3D4E5F", "3D4E5F60"}}
	var exports []*twinExport
	for _, p := range pairs {
		for i := range p {
			doc := twinDoc(p[i], []map[string]any{twinNeighborEntry(p[1-i], -95, 7)}, nil)
			exp, err := parseTwinExport(doc, p[i])
			if err != nil {
				t.Fatalf("parseTwinExport: %v", err)
			}
			exports = append(exports, exp)
		}
	}
	g, err := buildTwinGraph(exports)
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}

	conn, err := twinAnalyzeConnectivity(writeTwinScenarioFile(t, g, 120000, nil), g)
	if err != nil {
		t.Fatalf("twinAnalyzeConnectivity: %v", err)
	}
	if len(conn.BaselineComponents) != 2 {
		t.Fatalf("baseline components %v, want two pieces", conn.BaselineComponents)
	}
	// Removing one member of a pair leaves the other alone, which is a piece of
	// its own but not a NEW break relative to a mesh that was already split:
	// the survivor could not reach the other pair before the removal either.
	for _, addr := range []string{"0A1B2C3D", "1B2C3D4E", "2C3D4E5F", "3D4E5F60"} {
		row := criticalityRow(t, conn, addr)
		if row.Components != 2 {
			t.Fatalf("removing %s leaves %d pieces, want 2", addr, row.Components)
		}
		if len(row.Isolated) != 0 {
			t.Fatalf("removing %s is reported as stranding %v, yet it strands nothing that "+
				"could reach anything before", addr, row.Isolated)
		}
	}

	report := twinReport(g, conn, nil, []string{"a", "b", "c", "d"})
	if !strings.Contains(report, "ALREADY in 2 disconnected pieces") {
		t.Fatalf("report does not lead with the existing partition:\n%s", report)
	}
	if !strings.Contains(report, "No single node's loss partitions this mesh.") {
		t.Fatalf("report calls an already-split mesh's nodes single points of failure:\n%s",
			report)
	}
}

func TestTwinCriticalityFindsACutNodeInsideAPartitionedImport(t *testing.T) {
	// Two pieces: a three-node line (0A - 1B - 2C) and an unrelated pair
	// (3D - 4E). Only 1B2C3D4E is a cut node, and it strands exactly one node,
	// not everything outside the surviving piece it happens to be measured
	// against.
	pairs := [][2]string{
		{"0A1B2C3D", "1B2C3D4E"}, {"1B2C3D4E", "2C3D4E5F"}, {"3D4E5F60", "4E5F6071"},
	}
	byNode := map[string][]map[string]any{}
	for _, p := range pairs {
		byNode[p[0]] = append(byNode[p[0]], twinNeighborEntry(p[1], -95, 7))
		byNode[p[1]] = append(byNode[p[1]], twinNeighborEntry(p[0], -95, 7))
	}
	var exports []*twinExport
	for _, addr := range []string{"0A1B2C3D", "1B2C3D4E", "2C3D4E5F", "3D4E5F60", "4E5F6071"} {
		exports = append(exports, twinExportWith(t, addr, byNode[addr]))
	}
	g, err := buildTwinGraph(exports)
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	conn, err := twinAnalyzeConnectivity(writeTwinScenarioFile(t, g, 120000, nil), g)
	if err != nil {
		t.Fatalf("twinAnalyzeConnectivity: %v", err)
	}
	if len(conn.BaselineComponents) != 2 {
		t.Fatalf("baseline components %v, want two", conn.BaselineComponents)
	}

	middle := criticalityRow(t, conn, "1B2C3D4E")
	if !equalStrings(middle.Isolated, []string{"2C3D4E5F"}) {
		t.Fatalf("the cut node strands %v, want just the node behind it", middle.Isolated)
	}
	for _, addr := range []string{"0A1B2C3D", "2C3D4E5F", "3D4E5F60", "4E5F6071"} {
		if row := criticalityRow(t, conn, addr); len(row.Isolated) != 0 {
			t.Fatalf("leaf %s is reported as stranding %v", addr, row.Isolated)
		}
	}

	report := twinReport(g, conn, nil, []string{"a"})
	if !strings.Contains(report, "1 node(s) are single points of failure") {
		t.Fatalf("report does not count exactly one cut node:\n%s", report)
	}
}

func TestTwinCapacityProbeRunsOneScenarioPerRate(t *testing.T) {
	g := twinGraphFromDocs(t)
	dir := t.TempDir()
	// A short window and two rates: this checks the probe's bookkeeping, not
	// the mesh's capacity, and every extra rate is another full scenario run.
	probe, err := twinRunCapacityProbe(g, dir, 3, 120000, []float64{2, 6})
	if err != nil {
		t.Fatalf("twinRunCapacityProbe: %v", err)
	}
	if len(probe.Points) != 2 {
		t.Fatalf("%d points, want 2", len(probe.Points))
	}
	if probe.DurationMs != 120000 || probe.Seed != 3 {
		t.Fatalf("probe conditions %+v", probe)
	}

	for i, want := range []float64{2, 6} {
		pt := probe.Points[i]
		if pt.MsgsPerMin != want {
			t.Fatalf("point %d is %g msgs/min, want %g", i, pt.MsgsPerMin, want)
		}
		if pt.Scripted != len(twinTrafficEvents(g.Addresses(), 120000, want)) {
			t.Fatalf("point %g scripted %d messages", want, pt.Scripted)
		}
		// Every scripted message ends in exactly one terminal state, which is
		// what makes the delivery rate a rate of something.
		if total := pt.Delivered + pt.Dropped + pt.Undelivered; total != uint64(pt.Scripted) {
			t.Fatalf("point %g: %d terminal states for %d scripted messages",
				want, total, pt.Scripted)
		}
		if pt.DeliveryRate < 0 || pt.DeliveryRate > 1 {
			t.Fatalf("point %g delivery rate %g", want, pt.DeliveryRate)
		}
		if pt.ChannelUtilPct <= 0 {
			t.Fatalf("point %g reports no channel utilization, so no frame flew", want)
		}
	}
	// A scenario file per rate is left behind for the operator to re-run.
	for _, name := range []string{"capacity-2.json", "capacity-6.json"} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Fatalf("scenario %s: %v", name, err)
		}
	}
}

func TestTwinKneeIsMeasuredFromThePeakNotTheBottom(t *testing.T) {
	cases := []struct {
		name          string
		rates         []float64
		delivery      []float64
		wantPeak      float64
		wantKnee      float64
		wantSaturated float64
		wantBelow     []float64
	}{
		{
			name:          "a plain collapse",
			rates:         []float64{1, 2, 5, 10},
			delivery:      []float64{1.0, 0.98, 0.5, 0.1},
			wantPeak:      1,
			wantKnee:      2,
			wantSaturated: 5,
		},
		{
			name:          "delivery still holding at the top of the ramp",
			rates:         []float64{1, 2, 5},
			delivery:      []float64{0.9, 0.92, 0.9},
			wantPeak:      2,
			wantKnee:      5,
			wantSaturated: 0,
		},
		{
			// A ramp that climbs first: reading it from the bottom would call
			// the lowest rate saturated, which is the opposite of the truth.
			name:          "a ramp that climbs before it falls",
			rates:         []float64{1, 2, 5, 10, 20},
			delivery:      []float64{0.4, 0.55, 0.7, 0.72, 0.3},
			wantPeak:      10,
			wantKnee:      10,
			wantSaturated: 20,
			wantBelow:     []float64{1, 2},
		},
		{
			name:          "nothing delivered anywhere",
			rates:         []float64{1, 2},
			delivery:      []float64{0, 0},
			wantPeak:      1,
			wantKnee:      2,
			wantSaturated: 0,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			c := &twinCapacity{}
			for i, r := range tc.rates {
				c.Points = append(c.Points, twinCapacityPoint{
					MsgsPerMin: r, DeliveryRate: tc.delivery[i],
				})
			}
			c.findKnee()
			if c.PeakMsgsPerMin != tc.wantPeak {
				t.Fatalf("peak %g, want %g", c.PeakMsgsPerMin, tc.wantPeak)
			}
			if c.KneeMsgsPerMin != tc.wantKnee {
				t.Fatalf("knee %g, want %g", c.KneeMsgsPerMin, tc.wantKnee)
			}
			if c.SaturatedMsgsPerMin != tc.wantSaturated {
				t.Fatalf("saturated %g, want %g", c.SaturatedMsgsPerMin, tc.wantSaturated)
			}
			if len(c.BelowBarUnderPeak) != len(tc.wantBelow) {
				t.Fatalf("below-bar rates %v, want %v", c.BelowBarUnderPeak, tc.wantBelow)
			}
			for i := range tc.wantBelow {
				if c.BelowBarUnderPeak[i] != tc.wantBelow[i] {
					t.Fatalf("below-bar rates %v, want %v", c.BelowBarUnderPeak, tc.wantBelow)
				}
			}
		})
	}
}

func TestTwinReportStatesItsBoundsAndItsGaps(t *testing.T) {
	g := twinGraphFromDocs(t)
	conn, err := twinAnalyzeConnectivity(writeTwinScenarioFile(t, g, 120000, nil), g)
	if err != nil {
		t.Fatalf("twinAnalyzeConnectivity: %v", err)
	}
	report := twinReport(g, conn, nil, []string{"basecamp.json", "creek.json", "ridge.json",
		"tower.json"})

	for _, want := range []string{
		"Every number below is simulation",
		"it does not predict propagation",
		// The one link no device reported has to be named, in both the link
		// table and the gaps section.
		"assumed reciprocal",
		"Nodes present only through other nodes' neighbour tables (1): 4E5F6071",
		"2 node(s) are single points of failure",
	} {
		if !strings.Contains(report, want) {
			t.Fatalf("report is missing %q:\n%s", want, report)
		}
	}
	// A report with no capacity probe must not imply one ran.
	if strings.Contains(report, "Saturation knee") {
		t.Fatalf("report claims a capacity result without a probe:\n%s", report)
	}
}

func TestTwinReportNamesAnEmptyGapSection(t *testing.T) {
	// Both ends of the one link exported, so nothing was assumed.
	var exports []*twinExport
	pair := [2]string{"0A1B2C3D", "3D4E5F60"}
	for i := range pair {
		doc := twinDoc(pair[i], []map[string]any{twinNeighborEntry(pair[1-i], -95, 7)}, nil)
		exp, err := parseTwinExport(doc, pair[i])
		if err != nil {
			t.Fatalf("parseTwinExport: %v", err)
		}
		exports = append(exports, exp)
	}
	g, err := buildTwinGraph(exports)
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	report := twinReport(g, nil, nil, []string{"a.json", "b.json"})
	if !strings.Contains(report, "None: every node exported") {
		t.Fatalf("report does not state a clean reconstruction:\n%s", report)
	}
}
