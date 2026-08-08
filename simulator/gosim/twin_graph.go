package main

// Mesh digital twin: merging per-node exports into one link graph.
//
// What a deployment can actually tell you about itself is a set of directed
// observations: node B heard node A at this RSSI and SNR. It cannot tell you
// where its nodes are, and inverting RSSI into a position would invent
// propagation physics nobody measured. The reconstruction is therefore the
// observations themselves: a directed link graph, with the radio driven from
// it rather than from geometry (see radio_config_t's link table in
// simulator/engine/sim_radio.h).
//
// What that reproduces and what it does not is spelled out in
// ../../docs/digital-twin.md; the short version is that a twin replays the
// links a mesh reported at one moment, and predicts nothing about links nobody
// has observed.

import (
	"fmt"
	"sort"
)

// twinNode is one node of the reconstructed mesh.
type twinNode struct {
	// Address is the device address, 8 uppercase hex digits, and doubles as
	// the node's scenario id.
	Address string `json:"address"`
	Name    string `json:"name,omitempty"`
	// Exported is true when this node contributed an export file of its own.
	// A node that is only ever named by someone else's neighbor table is in
	// the graph on one-sided evidence.
	Exported bool   `json:"exported"`
	Firmware string `json:"firmware_version,omitempty"`
	Hardware string `json:"hardware,omitempty"`
	UptimeS  uint64 `json:"uptime_s"`
	TxPower  int    `json:"tx_power_dbm"`
}

// twinLink is one directed link: frames sent by From are heard at To.
type twinLink struct {
	From string `json:"from"`
	To   string `json:"to"`
	RSSI int    `json:"rssi"`
	SNR  int    `json:"snr"`
	// Observed is true when To's own export listed From as a neighbor, so a
	// device really did report hearing this transmitter. False means the
	// direction was filled in by reciprocity because To never exported (see
	// fillReciprocal); the report names every such link.
	Observed bool `json:"observed"`
	// LastSeenMs is the age of the observation as the reporting node measured
	// it, and is what arbitrates when two exports describe the same direction.
	LastSeenMs uint64 `json:"last_seen_ms"`
}

// twinGraph is the merged reconstruction: nodes, directed links, the PHY every
// export agreed on, and the honest record of what the merge had to assume.
type twinGraph struct {
	Nodes []twinNode
	Links []twinLink
	Radio twinExportRadio
	// RouteOnlyAddrs are addresses that appear only in routing tables, never
	// as an exporter or in anyone's neighbor table. They are real nodes of the
	// real mesh, but no export carries a link to them, so the twin cannot
	// place them and leaves them out rather than inventing connectivity.
	RouteOnlyAddrs []string
	// Notes records every merge decision an operator should know about:
	// reciprocity fills, conflicting observations of the same direction,
	// duplicate exports, per-node transmit-power differences.
	Notes []string
}

// buildTwinGraph merges parsed exports, in the order given, into a link graph.
func buildTwinGraph(exports []*twinExport) (*twinGraph, error) {
	if len(exports) == 0 {
		return nil, fmt.Errorf("no exports to import")
	}

	g := &twinGraph{Radio: exports[0].Radio}
	nodes := map[string]*twinNode{}
	links := map[string]*twinLink{}
	routeSeen := map[string]bool{}

	node := func(addr string) *twinNode {
		n, ok := nodes[addr]
		if !ok {
			n = &twinNode{Address: addr}
			nodes[addr] = n
		}
		return n
	}

	for _, exp := range exports {
		if err := g.adoptRadio(exp); err != nil {
			return nil, err
		}

		self := node(exp.Node.Address)
		if self.Exported {
			g.Notes = append(g.Notes, fmt.Sprintf(
				"node %s exported more than once; the later document's observations win where "+
					"they are fresher", exp.Node.Address))
		}
		self.Exported = true
		self.Name = exp.Node.Name
		self.Firmware = exp.Node.FirmwareVersion
		self.Hardware = exp.Node.Hardware
		self.UptimeS = exp.Node.UptimeS
		self.TxPower = exp.Radio.TxPowerDBm

		for _, nb := range exp.Neighbors {
			peer := node(nb.Address)
			if peer.Name == "" {
				peer.Name = nb.Name
			}
			// The exporter measured this RSSI, so the observation describes
			// the neighbor-to-exporter direction.
			key := nb.Address + ">" + exp.Node.Address
			cand := twinLink{
				From:       nb.Address,
				To:         exp.Node.Address,
				RSSI:       nb.RSSI,
				SNR:        nb.SNR,
				Observed:   true,
				LastSeenMs: nb.LastSeenMs,
			}
			prev, exists := links[key]
			if !exists {
				links[key] = &cand
				continue
			}
			// Two documents describe the same direction, which happens when a
			// node is exported more than once. The fresher observation wins;
			// an exact tie keeps the earlier file's, so a merge is
			// deterministic in argument order.
			if cand != *prev {
				g.Notes = append(g.Notes, fmt.Sprintf(
					"link %s -> %s observed twice (%d dBm / %d dB, %d ms ago, and %d dBm / %d dB, "+
						"%d ms ago); kept the fresher reading", cand.From, cand.To, prev.RSSI,
					prev.SNR, prev.LastSeenMs, cand.RSSI, cand.SNR, cand.LastSeenMs))
			}
			if cand.LastSeenMs < prev.LastSeenMs {
				*prev = cand
			}
		}

		for _, r := range exp.Routes {
			routeSeen[r.Dest] = true
			routeSeen[r.NextHop] = true
		}
	}

	for _, l := range links {
		g.Links = append(g.Links, *l)
	}
	g.fillReciprocal()

	for _, n := range nodes {
		g.Nodes = append(g.Nodes, *n)
	}
	sort.Slice(g.Nodes, func(i, j int) bool { return g.Nodes[i].Address < g.Nodes[j].Address })
	sort.Slice(g.Links, func(i, j int) bool {
		if g.Links[i].From != g.Links[j].From {
			return g.Links[i].From < g.Links[j].From
		}
		return g.Links[i].To < g.Links[j].To
	})

	for addr := range routeSeen {
		if _, known := nodes[addr]; !known {
			g.RouteOnlyAddrs = append(g.RouteOnlyAddrs, addr)
		}
	}
	sort.Strings(g.RouteOnlyAddrs)

	txPowers := map[int][]string{}
	for _, n := range g.Nodes {
		if n.Exported {
			txPowers[n.TxPower] = append(txPowers[n.TxPower], n.Address)
		}
	}
	if len(txPowers) > 1 {
		g.Notes = append(g.Notes, "exports disagree on transmit power; link mode reads received "+
			"power from the observations themselves, so this does not change the reconstruction")
	}

	if len(g.Links) == 0 {
		return nil, fmt.Errorf("no links: every export has an empty neighbor table, so there is "+
			"no observed mesh to reconstruct (%d nodes)", len(g.Nodes))
	}
	sort.Strings(g.Notes)
	return g, nil
}

// adoptRadio checks one export's PHY against the graph's. The simulated medium
// is a single channel with a single PHY, so a fleet that genuinely disagrees
// about spreading factor, bandwidth, coding rate or frequency is not one mesh
// and cannot be modeled as one: that is a hard refusal, not a warning.
// Transmit power is exempt: it legitimately varies per node and link mode
// never reads it, because received power comes from the observations.
func (g *twinGraph) adoptRadio(exp *twinExport) error {
	r := g.Radio
	e := exp.Radio
	mismatch := func(field string, want, got any) error {
		return fmt.Errorf("export from %s disagrees on radio.%s (%v against %v): a twin models one "+
			"channel with one PHY, so exports from differently configured nodes cannot be merged",
			exp.Node.Address, field, got, want)
	}
	switch {
	case r.SF != e.SF:
		return mismatch("sf", r.SF, e.SF)
	case r.BWHz != e.BWHz:
		return mismatch("bw_hz", r.BWHz, e.BWHz)
	case r.CodingRate != e.CodingRate:
		return mismatch("coding_rate", r.CodingRate, e.CodingRate)
	case r.FrequencyMHz != e.FrequencyMHz:
		return mismatch("frequency_mhz", r.FrequencyMHz, e.FrequencyMHz)
	case r.Region != e.Region:
		return mismatch("region", r.Region, e.Region)
	}
	return nil
}

// fillReciprocal supplies the missing direction of every one-sided link.
//
// A link only both of whose directions were observed needs two exports. When
// only one end exported, the reverse direction is unknown, and leaving it out
// would model a one-way link no protocol exchange can cross: the twin would
// report a mesh far more broken than the one that is running. Assuming
// reciprocity at the same RSSI and SNR is the smaller and more honest error,
// and every filled direction is marked Observed=false so the report can name
// it and the operator can close the gap by exporting from the other end.
func (g *twinGraph) fillReciprocal() {
	have := map[string]bool{}
	for _, l := range g.Links {
		have[l.From+">"+l.To] = true
	}
	var added []twinLink
	for _, l := range g.Links {
		if have[l.To+">"+l.From] {
			continue
		}
		have[l.To+">"+l.From] = true
		added = append(added, twinLink{
			From:       l.To,
			To:         l.From,
			RSSI:       l.RSSI,
			SNR:        l.SNR,
			Observed:   false,
			LastSeenMs: l.LastSeenMs,
		})
	}
	g.Links = append(g.Links, added...)
}

// Addresses returns every node address in the graph, in scenario order.
func (g *twinGraph) Addresses() []string {
	out := make([]string, len(g.Nodes))
	for i, n := range g.Nodes {
		out[i] = n.Address
	}
	return out
}

// NodeByAddress returns the node with this address, or nil.
func (g *twinGraph) NodeByAddress(addr string) *twinNode {
	for i := range g.Nodes {
		if g.Nodes[i].Address == addr {
			return &g.Nodes[i]
		}
	}
	return nil
}

// Degree counts the links leaving a node (which, after fillReciprocal, is also
// the number of nodes it can reach directly).
func (g *twinGraph) Degree(addr string) int {
	n := 0
	for _, l := range g.Links {
		if l.From == addr {
			n++
		}
	}
	return n
}

// UnobservedLinks lists the directions no device reported, i.e. the ones
// fillReciprocal supplied.
func (g *twinGraph) UnobservedLinks() []twinLink {
	var out []twinLink
	for _, l := range g.Links {
		if !l.Observed {
			out = append(out, l)
		}
	}
	return out
}

// UnexportedNodes lists nodes present only through other nodes' observations.
func (g *twinGraph) UnexportedNodes() []string {
	var out []string
	for _, n := range g.Nodes {
		if !n.Exported {
			out = append(out, n.Address)
		}
	}
	return out
}
