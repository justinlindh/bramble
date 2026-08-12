package main

// Mesh digital twin: the importer.
//
// These cases pin the two things the reconstruction rests on: that a document
// the parser does not fully understand is refused rather than half-read, and
// that merging several nodes' views produces exactly the directed link graph
// those nodes reported and nothing more.
//
// The fixtures under testdata/twin are the same files docs/digital-twin.md
// works through, so the worked example in the docs and the tested behaviour
// cannot drift apart.

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// twinFixture is one of the committed example exports.
func twinFixture(t *testing.T, name string) *twinExport {
	t.Helper()
	exp, err := loadTwinExport(filepath.Join("testdata", "twin", name+".json"))
	if err != nil {
		t.Fatalf("loadTwinExport(%s): %v", name, err)
	}
	return exp
}

// twinDoc builds an export document as a map, so a case can corrupt exactly one
// field and leave the rest schema-correct.
func twinDoc(addr string, neighbors []map[string]any, mutate func(map[string]any)) []byte {
	doc := map[string]any{
		"twin_schema": 1,
		"node": map[string]any{
			"address":          addr,
			"firmware_version": "0.9.3",
			"protocol_version": "0.5.0",
			"hardware":         "heltec_v3",
			"uptime_s":         1200,
		},
		"radio": map[string]any{
			"frequency_mhz":       915.0,
			"sf":                  9,
			"bw_hz":               125000,
			"coding_rate":         1,
			"tx_power_dbm":        22,
			"region":              "US915",
			"regulatory":          "FCC Part 15.247",
			"max_duty_cycle_pct":  100,
			"duty_cycle_enforced": false,
		},
		"neighbors": neighbors,
		"routes":    []map[string]any{},
	}
	if mutate != nil {
		mutate(doc)
	}
	b, err := json.Marshal(doc)
	if err != nil {
		panic(err)
	}
	return b
}

func twinNeighborEntry(addr string, rssi, snr int) map[string]any {
	return map[string]any{
		"address": addr, "rssi": rssi, "snr": snr,
		"deliveryRate": 250, "airtimeRemaining": 95, "last_seen_ms": 5000,
	}
}

// linkKeys renders a graph's links as sorted "FROM>TO@rssi/snr:source" strings,
// the form the comparisons below read.
func linkKeys(g *twinGraph) []string {
	out := make([]string, 0, len(g.Links))
	for _, l := range g.Links {
		src := "observed"
		if !l.Observed {
			src = "assumed"
		}
		out = append(out, fmt.Sprintf("%s>%s@%d/%d:%s", l.From, l.To, l.RSSI, l.SNR, src))
	}
	sort.Strings(out)
	return out
}

func TestTwinImportParsesTheFixtureExports(t *testing.T) {
	exp := twinFixture(t, "tower")
	if exp.Node.Address != "3D4E5F60" || exp.Node.Name != "tower" {
		t.Fatalf("node identity: %+v", exp.Node)
	}
	if exp.Radio.SF != 9 || exp.Radio.BWHz != 125000 || exp.Radio.CodingRate != 1 {
		t.Fatalf("radio PHY: %+v", exp.Radio)
	}
	if len(exp.Neighbors) != 3 || len(exp.Routes) != 4 {
		t.Fatalf("neighbors %d routes %d", len(exp.Neighbors), len(exp.Routes))
	}
	if exp.Neighbors[0].Address != "0A1B2C3D" || exp.Neighbors[0].RSSI != -95 {
		t.Fatalf("first neighbor: %+v", exp.Neighbors[0])
	}
}

func TestTwinImportAcceptsAJSONRPCEnvelope(t *testing.T) {
	inner := twinDoc("0A1B2C3D", []map[string]any{twinNeighborEntry("3D4E5F60", -92, 9)}, nil)
	wrapped := []byte(`{"jsonrpc":"2.0","id":7,"result":` + string(inner) + `}`)

	exp, err := parseTwinExport(wrapped, "wrapped")
	if err != nil {
		t.Fatalf("parseTwinExport: %v", err)
	}
	if exp.Node.Address != "0A1B2C3D" || len(exp.Neighbors) != 1 {
		t.Fatalf("envelope unwrapped wrong: %+v", exp)
	}
}

func TestTwinImportRefusesAnRPCErrorResponse(t *testing.T) {
	_, err := parseTwinExport(
		[]byte(`{"jsonrpc":"2.0","id":7,"error":{"code":-32601,"message":"no such method"}}`),
		"errdoc")
	if err == nil || !strings.Contains(err.Error(), "no such method") {
		t.Fatalf("want the RPC error surfaced, got %v", err)
	}
}

func TestTwinImportRefusesAnUnknownSchemaVersion(t *testing.T) {
	doc := twinDoc("0A1B2C3D", nil, func(m map[string]any) { m["twin_schema"] = 2 })
	_, err := parseTwinExport(doc, "future")
	if err == nil || !strings.Contains(err.Error(), "twin_schema 2") {
		t.Fatalf("want a schema-version refusal, got %v", err)
	}
}

func TestTwinImportRefusesMalformedObservations(t *testing.T) {
	cases := []struct {
		name string
		doc  []byte
		want string
	}{
		{
			name: "unreceivable rssi",
			doc: twinDoc("0A1B2C3D",
				[]map[string]any{twinNeighborEntry("3D4E5F60", 0, 9)}, nil),
			want: "not a receivable",
		},
		{
			name: "node listed as its own neighbour",
			doc: twinDoc("0A1B2C3D",
				[]map[string]any{twinNeighborEntry("0A1B2C3D", -90, 9)}, nil),
			want: "as its own neighbor",
		},
		{
			name: "null address",
			doc:  twinDoc("00000000", nil, nil),
			want: "null address",
		},
		{
			name: "address is not hex",
			doc:  twinDoc("basecamp", nil, nil),
			want: "not a 32-bit hex address",
		},
		{
			name: "spreading factor off the LoRa scale",
			doc: twinDoc("0A1B2C3D", nil, func(m map[string]any) {
				m["radio"].(map[string]any)["sf"] = 13
			}),
			want: "outside the LoRa range",
		},
		{
			name: "coding rate off the scale",
			doc: twinDoc("0A1B2C3D", nil, func(m map[string]any) {
				m["radio"].(map[string]any)["coding_rate"] = 9
			}),
			want: "coding_rate 9",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := parseTwinExport(tc.doc, "doc")
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("want an error containing %q, got %v", tc.want, err)
			}
		})
	}
}

func TestTwinImportNormalizesAddressSpelling(t *testing.T) {
	doc := twinDoc("0x0a1b2c3d", []map[string]any{twinNeighborEntry(" 3d4e5f60 ", -92, 9)}, nil)
	exp, err := parseTwinExport(doc, "doc")
	if err != nil {
		t.Fatalf("parseTwinExport: %v", err)
	}
	if exp.Node.Address != "0A1B2C3D" {
		t.Fatalf("node address %q", exp.Node.Address)
	}
	if exp.Neighbors[0].Address != "3D4E5F60" {
		t.Fatalf("neighbor address %q", exp.Neighbors[0].Address)
	}
}

func TestTwinMergeBuildsTheReportedLinkGraph(t *testing.T) {
	g, err := buildTwinGraph([]*twinExport{
		twinFixture(t, "basecamp"), twinFixture(t, "creek"),
		twinFixture(t, "ridge"), twinFixture(t, "tower"),
	})
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}

	wantNodes := []string{"0A1B2C3D", "1B2C3D4E", "2C3D4E5F", "3D4E5F60", "4E5F6071"}
	if got := g.Addresses(); !equalStrings(got, wantNodes) {
		t.Fatalf("nodes %v, want %v", got, wantNodes)
	}

	// Seven directions were reported by a device; the eighth is the reverse of
	// the only link whose far end never exported, filled by reciprocity.
	want := []string{
		"0A1B2C3D>3D4E5F60@-95/8:observed",
		"1B2C3D4E>3D4E5F60@-99/5:observed",
		"1B2C3D4E>4E5F6071@-88/11:assumed",
		"2C3D4E5F>3D4E5F60@-110/1:observed",
		"3D4E5F60>0A1B2C3D@-92/9:observed",
		"3D4E5F60>1B2C3D4E@-101/4:observed",
		"3D4E5F60>2C3D4E5F@-108/2:observed",
		"4E5F6071>1B2C3D4E@-88/11:observed",
	}
	if got := linkKeys(g); !equalStrings(got, want) {
		t.Fatalf("links\n got %v\nwant %v", got, want)
	}

	if got := g.UnexportedNodes(); !equalStrings(got, []string{"4E5F6071"}) {
		t.Fatalf("unexported nodes %v", got)
	}
	// north-cabin never exported, but ridge's neighbour table carried its name.
	if n := g.NodeByAddress("4E5F6071"); n == nil || n.Name != "north-cabin" {
		t.Fatalf("name learned from a neighbour table: %+v", n)
	}
	if len(g.RouteOnlyAddrs) != 0 {
		t.Fatalf("route-only addresses %v, want none", g.RouteOnlyAddrs)
	}
	if len(g.Notes) != 0 {
		t.Fatalf("merge notes %v, want none", g.Notes)
	}
}

func TestTwinMergeIsIndependentOfFileOrder(t *testing.T) {
	forward, err := buildTwinGraph([]*twinExport{
		twinFixture(t, "basecamp"), twinFixture(t, "creek"),
		twinFixture(t, "ridge"), twinFixture(t, "tower"),
	})
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	reverse, err := buildTwinGraph([]*twinExport{
		twinFixture(t, "tower"), twinFixture(t, "ridge"),
		twinFixture(t, "creek"), twinFixture(t, "basecamp"),
	})
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	if !equalStrings(linkKeys(forward), linkKeys(reverse)) {
		t.Fatalf("argument order changed the graph:\n%v\n%v",
			linkKeys(forward), linkKeys(reverse))
	}
	if !equalStrings(forward.Addresses(), reverse.Addresses()) {
		t.Fatalf("argument order changed the node set")
	}
}

func TestTwinMergeKeepsTheFresherOfTwoConflictingObservations(t *testing.T) {
	stale := twinDoc("0A1B2C3D", []map[string]any{
		{"address": "3D4E5F60", "rssi": -120, "snr": 1,
			"deliveryRate": 200, "airtimeRemaining": 90, "last_seen_ms": 90000},
	}, nil)
	fresh := twinDoc("0A1B2C3D", []map[string]any{
		{"address": "3D4E5F60", "rssi": -92, "snr": 9,
			"deliveryRate": 250, "airtimeRemaining": 95, "last_seen_ms": 4000},
	}, nil)

	staleExp, err := parseTwinExport(stale, "stale")
	if err != nil {
		t.Fatalf("parse stale: %v", err)
	}
	freshExp, err := parseTwinExport(fresh, "fresh")
	if err != nil {
		t.Fatalf("parse fresh: %v", err)
	}

	// Both orders must land on the fresher reading: an operator who exported a
	// node twice cannot be expected to pass the files in any particular order.
	for _, order := range [][]*twinExport{{staleExp, freshExp}, {freshExp, staleExp}} {
		g, err := buildTwinGraph(order)
		if err != nil {
			t.Fatalf("buildTwinGraph: %v", err)
		}
		var link *twinLink
		for i := range g.Links {
			if g.Links[i].From == "3D4E5F60" && g.Links[i].To == "0A1B2C3D" {
				link = &g.Links[i]
			}
		}
		if link == nil {
			t.Fatalf("link 3D4E5F60 -> 0A1B2C3D missing")
		}
		if link.RSSI != -92 || link.SNR != 9 {
			t.Fatalf("kept %d dBm / %d dB, want the fresher -92 / 9", link.RSSI, link.SNR)
		}
		if len(g.Notes) != 2 {
			t.Fatalf("notes %v, want one for the duplicate export and one for the conflict",
				g.Notes)
		}
		joined := strings.Join(g.Notes, "\n")
		if !strings.Contains(joined, "exported more than once") ||
			!strings.Contains(joined, "observed twice") {
			t.Fatalf("notes do not name both decisions: %v", g.Notes)
		}
	}
}

func TestTwinMergeRefusesExportsFromDifferentPHYs(t *testing.T) {
	a, err := parseTwinExport(
		twinDoc("0A1B2C3D", []map[string]any{twinNeighborEntry("3D4E5F60", -92, 9)}, nil), "a")
	if err != nil {
		t.Fatalf("parse a: %v", err)
	}
	b, err := parseTwinExport(
		twinDoc("3D4E5F60", []map[string]any{twinNeighborEntry("0A1B2C3D", -95, 8)},
			func(m map[string]any) { m["radio"].(map[string]any)["sf"] = 11 }), "b")
	if err != nil {
		t.Fatalf("parse b: %v", err)
	}
	if _, err := buildTwinGraph([]*twinExport{a, b}); err == nil ||
		!strings.Contains(err.Error(), "radio.sf") {
		t.Fatalf("want a PHY-mismatch refusal, got %v", err)
	}
}

func TestTwinMergeAcceptsDifferingTransmitPower(t *testing.T) {
	a, err := parseTwinExport(
		twinDoc("0A1B2C3D", []map[string]any{twinNeighborEntry("3D4E5F60", -92, 9)}, nil), "a")
	if err != nil {
		t.Fatalf("parse a: %v", err)
	}
	b, err := parseTwinExport(
		twinDoc("3D4E5F60", []map[string]any{twinNeighborEntry("0A1B2C3D", -95, 8)},
			func(m map[string]any) { m["radio"].(map[string]any)["tx_power_dbm"] = 14 }), "b")
	if err != nil {
		t.Fatalf("parse b: %v", err)
	}
	g, err := buildTwinGraph([]*twinExport{a, b})
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	if len(g.Notes) != 1 || !strings.Contains(g.Notes[0], "transmit power") {
		t.Fatalf("notes %v, want one about transmit power", g.Notes)
	}
}

func TestTwinMergeReportsAddressesKnownOnlyFromRoutes(t *testing.T) {
	doc := twinDoc("0A1B2C3D", []map[string]any{twinNeighborEntry("3D4E5F60", -92, 9)},
		func(m map[string]any) {
			m["routes"] = []map[string]any{
				{"dest": "5F607182", "next_hop": "3D4E5F60", "hop_count": 3,
					"metric": 90, "state": "active", "use_count": 4},
			}
		})
	exp, err := parseTwinExport(doc, "doc")
	if err != nil {
		t.Fatalf("parseTwinExport: %v", err)
	}
	g, err := buildTwinGraph([]*twinExport{exp})
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	if !equalStrings(g.RouteOnlyAddrs, []string{"5F607182"}) {
		t.Fatalf("route-only addresses %v", g.RouteOnlyAddrs)
	}
	// A routed-but-unheard address is named, never placed: inventing a link to
	// it would answer capacity questions about a mesh nobody observed.
	for _, a := range g.Addresses() {
		if a == "5F607182" {
			t.Fatalf("route-only address became a node")
		}
	}
}

func TestTwinMergeRefusesAnImportWithNoObservedLink(t *testing.T) {
	exp, err := parseTwinExport(twinDoc("0A1B2C3D", nil, nil), "lonely")
	if err != nil {
		t.Fatalf("parseTwinExport: %v", err)
	}
	if _, err := buildTwinGraph([]*twinExport{exp}); err == nil ||
		!strings.Contains(err.Error(), "no links") {
		t.Fatalf("want a no-links refusal, got %v", err)
	}
}

func TestTwinScenarioRendersTheGraphAsALinkModeScenario(t *testing.T) {
	g, err := buildTwinGraph([]*twinExport{
		twinFixture(t, "basecamp"), twinFixture(t, "creek"),
		twinFixture(t, "ridge"), twinFixture(t, "tower"),
	})
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	sc := buildTwinScenario(g, "twin-test", 7, 120000, nil)
	if len(sc.Nodes) != 5 || len(sc.Links) != 8 {
		t.Fatalf("scenario has %d nodes and %d links", len(sc.Nodes), len(sc.Links))
	}
	if sc.Radio.SF != 9 || sc.Radio.BWHz != 125000 || sc.Radio.CR != 1 {
		t.Fatalf("scenario PHY %+v", sc.Radio)
	}
	// US915's plan does not enforce a duty cycle, so the twin applies none.
	if sc.Radio.DutyCyclePct != nil {
		t.Fatalf("duty cycle %d applied for an advisory plan", *sc.Radio.DutyCyclePct)
	}

	// The scenario has to load: an unknown node id or a zero RSSI in the links
	// block is a hard failure in sim_scenario.c, so this also checks that every
	// link the merge produced names nodes the scenario declares.
	dir := t.TempDir()
	path := filepath.Join(dir, "twin.json")
	data, err := sc.JSON()
	if err != nil {
		t.Fatalf("scenario JSON: %v", err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatalf("write scenario: %v", err)
	}
	topo, err := loadTwinTopology(path)
	if err != nil {
		t.Fatalf("loadTwinTopology: %v", err)
	}
	defer topo.free()
	if len(topo.ids) != 5 {
		t.Fatalf("loaded %d nodes", len(topo.ids))
	}
}

func TestTwinScenarioAppliesAnEnforcedDutyCycle(t *testing.T) {
	eu := twinDoc("0A1B2C3D", []map[string]any{twinNeighborEntry("3D4E5F60", -92, 9)},
		func(m map[string]any) {
			r := m["radio"].(map[string]any)
			r["region"] = "EU868"
			r["regulatory"] = "ETSI EN 300.220"
			r["frequency_mhz"] = 868.1
			r["max_duty_cycle_pct"] = 1
			r["duty_cycle_enforced"] = true
		})
	exp, err := parseTwinExport(eu, "eu")
	if err != nil {
		t.Fatalf("parseTwinExport: %v", err)
	}
	g, err := buildTwinGraph([]*twinExport{exp})
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	sc := buildTwinScenario(g, "twin-eu", 1, 120000, nil)
	if sc.Radio.DutyCyclePct == nil || *sc.Radio.DutyCyclePct != 1 {
		t.Fatalf("enforced 1%% plan did not reach the scenario: %+v", sc.Radio)
	}
}

func TestTwinReportAgreesWithScenarioOnAMalformedZeroCap(t *testing.T) {
	// enforced=true but max_duty_cycle_pct=0 is malformed: a 0% cap is not a
	// real ceiling, so the scenario builder leaves the twin uncapped. The
	// report must not then claim the cap was applied to every node; both sides
	// go through appliesDutyCap, so they agree.
	doc := twinDoc("0A1B2C3D", []map[string]any{twinNeighborEntry("3D4E5F60", -92, 9)},
		func(m map[string]any) {
			r := m["radio"].(map[string]any)
			r["max_duty_cycle_pct"] = 0
			r["duty_cycle_enforced"] = true
		})
	exp, err := parseTwinExport(doc, "zero")
	if err != nil {
		t.Fatalf("parseTwinExport: %v", err)
	}
	g, err := buildTwinGraph([]*twinExport{exp})
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	if sc := buildTwinScenario(g, "twin-zero", 1, 120000, nil); sc.Radio.DutyCyclePct != nil {
		t.Fatalf("a 0%% cap must not reach the scenario: %+v", sc.Radio)
	}
	report := twinReport(g, nil, nil, []string{"zero.json"})
	if strings.Contains(report, "applied to every node") {
		t.Fatalf("report claims a 0%% cap was applied while the scenario dropped it:\n%s", report)
	}
	if !strings.Contains(report, "advisory") {
		t.Fatalf("report does not call the uncapped plan advisory:\n%s", report)
	}
}

func TestTwinTrafficEventsFollowThePublishedConstruction(t *testing.T) {
	addrs := []string{"AAAA0001", "AAAA0002", "AAAA0003", "AAAA0004"}
	events := twinTrafficEvents(addrs, 600000, 6)
	// One message every 10 s, from 10 s up to but not including 590 s.
	if len(events) != 58 {
		t.Fatalf("%d events, want 58", len(events))
	}
	if events[0].AtMs != 10000 || events[1].AtMs != 20000 {
		t.Fatalf("event spacing: %d then %d", events[0].AtMs, events[1].AtMs)
	}
	for i, e := range events {
		if e.Type != "send_message" {
			t.Fatalf("event %d type %q", i, e.Type)
		}
		if e.Src == e.Dest {
			t.Fatalf("event %d addresses itself", i)
		}
	}
	// A rate the mesh cannot be probed at yields no traffic rather than a
	// divide by zero or an infinite loop.
	if got := twinTrafficEvents(addrs, 600000, 0); len(got) != 0 {
		t.Fatalf("zero rate produced %d events", len(got))
	}
	if got := twinTrafficEvents(addrs[:1], 600000, 6); len(got) != 0 {
		t.Fatalf("single-node mesh produced %d events", len(got))
	}
	// A two-node mesh is the one fleet size where "half the fleet away" names
	// the sender itself; it still has to be probed, alternating direction.
	pair := twinTrafficEvents(addrs[:2], 600000, 6)
	if len(pair) != 58 {
		t.Fatalf("two-node mesh produced %d events, want 58", len(pair))
	}
	if pair[0].Src != addrs[0] || pair[0].Dest != addrs[1] ||
		pair[1].Src != addrs[1] || pair[1].Dest != addrs[0] {
		t.Fatalf("two-node traffic does not alternate: %+v %+v", pair[0], pair[1])
	}
}

func TestParseTwinRatesRequiresAnAscendingRamp(t *testing.T) {
	got, err := parseTwinRates("1, 2,5 ,10")
	if err != nil {
		t.Fatalf("parseTwinRates: %v", err)
	}
	if len(got) != 4 || got[0] != 1 || got[3] != 10 {
		t.Fatalf("parsed %v", got)
	}
	for _, bad := range []string{"", "2,1", "1,1", "0", "-3", "5,abc"} {
		if _, err := parseTwinRates(bad); err == nil {
			t.Fatalf("parseTwinRates(%q) accepted an unusable ramp", bad)
		}
	}
}

func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// twinExportWith parses one export document, failing the test if it does not
// parse: the shorthand the reciprocity cases below are built from.
func twinExportWith(t *testing.T, addr string, neighbors []map[string]any) *twinExport {
	t.Helper()
	exp, err := parseTwinExport(twinDoc(addr, neighbors, nil), addr)
	if err != nil {
		t.Fatalf("parseTwinExport(%s): %v", addr, err)
	}
	return exp
}

func TestTwinMergeKeepsAOneWayLinkBothEndsExported(t *testing.T) {
	// 0A1B2C3D hears 3D4E5F60. 3D4E5F60 exported too, and its neighbour table
	// is empty: it does not hear 0A1B2C3D. That is measured evidence of a
	// one-way link, so the merge must not invent the reverse direction.
	g, err := buildTwinGraph([]*twinExport{
		twinExportWith(t, "0A1B2C3D", []map[string]any{twinNeighborEntry("3D4E5F60", -95, 7)}),
		twinExportWith(t, "3D4E5F60", nil),
	})
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	want := []string{"3D4E5F60>0A1B2C3D@-95/7:observed"}
	if got := linkKeys(g); !equalStrings(got, want) {
		t.Fatalf("links\n got %v\nwant %v", got, want)
	}
	if len(g.UnobservedLinks()) != 0 {
		t.Fatalf("a direction was assumed against the far end's own export: %v",
			g.UnobservedLinks())
	}
	if ow := g.OneWayLinks(); len(ow) != 1 || ow[0].From != "3D4E5F60" {
		t.Fatalf("one-way links %v, want the single measured asymmetry", ow)
	}

	// The report has to name it, and must not claim a clean reconstruction.
	report := twinReport(g, nil, nil, []string{"a.json", "b.json"})
	if !strings.Contains(report, "One-way links, heard at one end and not the other (1)") {
		t.Fatalf("report does not name the one-way link:\n%s", report)
	}
	if strings.Contains(report, "None: every node exported") {
		t.Fatalf("report calls an asymmetric reconstruction clean:\n%s", report)
	}
}

func TestTwinMergeFillsOnlyTheDirectionsNobodyCouldReport(t *testing.T) {
	// 0A1B2C3D and 2C3D4E5F both hear 1B2C3D4E, which exports and lists only
	// 0A1B2C3D. 4E5F6071 never exports at all. So 1B2C3D4E -> 2C3D4E5F stays
	// one-way on 1B2C3D4E's own evidence, while the direction toward the node
	// that never exported is filled by reciprocity.
	g, err := buildTwinGraph([]*twinExport{
		twinExportWith(t, "0A1B2C3D", []map[string]any{twinNeighborEntry("1B2C3D4E", -95, 7)}),
		twinExportWith(t, "2C3D4E5F", []map[string]any{twinNeighborEntry("1B2C3D4E", -99, 5)}),
		twinExportWith(t, "1B2C3D4E", []map[string]any{
			twinNeighborEntry("0A1B2C3D", -92, 9), twinNeighborEntry("4E5F6071", -88, 11)}),
	})
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	want := []string{
		"0A1B2C3D>1B2C3D4E@-92/9:observed",
		"1B2C3D4E>0A1B2C3D@-95/7:observed",
		"1B2C3D4E>2C3D4E5F@-99/5:observed",
		"1B2C3D4E>4E5F6071@-88/11:assumed",
		"4E5F6071>1B2C3D4E@-88/11:observed",
	}
	if got := linkKeys(g); !equalStrings(got, want) {
		t.Fatalf("links\n got %v\nwant %v", got, want)
	}
	ow := g.OneWayLinks()
	if len(ow) != 1 || ow[0].From != "1B2C3D4E" || ow[0].To != "2C3D4E5F" {
		t.Fatalf("one-way links %v, want 1B2C3D4E -> 2C3D4E5F only", ow)
	}
}

func TestTwinOneWayLinkLeavesTheEndsUnconnected(t *testing.T) {
	// radio_nodes_connected requires both directions, so a measured one-way
	// link is not a path: the partition traversal has to see two pieces, which
	// is the whole reason the merge must not invent the reverse direction.
	g, err := buildTwinGraph([]*twinExport{
		twinExportWith(t, "0A1B2C3D", []map[string]any{twinNeighborEntry("3D4E5F60", -95, 7)}),
		twinExportWith(t, "3D4E5F60", nil),
	})
	if err != nil {
		t.Fatalf("buildTwinGraph: %v", err)
	}
	conn, err := twinAnalyzeConnectivity(writeTwinScenarioFile(t, g, 120000, nil), g)
	if err != nil {
		t.Fatalf("twinAnalyzeConnectivity: %v", err)
	}
	if len(conn.BaselineComponents) != 2 {
		t.Fatalf("baseline components %v, want two: a one-way link joins nothing",
			conn.BaselineComponents)
	}
}
