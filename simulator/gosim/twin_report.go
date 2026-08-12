package main

// Mesh digital twin: the operator-facing report.
//
// The audience is somebody who has a mesh in the field and wants to know what
// it can carry and where it is fragile, so the report leads with the answers,
// shows the measurements behind them, and states plainly which parts of the
// reconstruction rest on assumption rather than observation.

import (
	"fmt"
	"sort"
	"strings"
)

// twinReport renders the whole analysis as plain text.
func twinReport(g *twinGraph, conn *twinConnectivity, probe *twinCapacity, sources []string) string {
	var b strings.Builder

	fmt.Fprintf(&b, "Bramble mesh digital twin\n")
	fmt.Fprintf(&b, "=========================\n\n")
	fmt.Fprintf(&b, "Every number below is simulation: the reconstructed mesh run through the\n")
	fmt.Fprintf(&b, "firmware's own protocol code and the simulator's collision model, not a\n")
	fmt.Fprintf(&b, "field measurement. The twin replays the links these nodes reported at the\n")
	fmt.Fprintf(&b, "moment of export; it does not predict propagation.\n\n")

	writeTwinSources(&b, g, sources)
	writeTwinLinks(&b, g)
	writeTwinCoverage(&b, g)
	if probe != nil {
		writeTwinCapacity(&b, probe)
	}
	if conn != nil {
		writeTwinCriticality(&b, conn)
	}
	return b.String()
}

func writeTwinSources(b *strings.Builder, g *twinGraph, sources []string) {
	fmt.Fprintf(b, "Imported mesh\n")
	fmt.Fprintf(b, "-------------\n\n")
	exported := 0
	for _, n := range g.Nodes {
		if n.Exported {
			exported++
		}
	}
	fmt.Fprintf(b, "%d export file(s), %d node(s), %d directed link(s).\n", len(sources),
		len(g.Nodes), len(g.Links))
	fmt.Fprintf(b, "%d of %d nodes exported their own view.\n\n", exported, len(g.Nodes))

	r := g.Radio
	fmt.Fprintf(b, "Radio: SF%d BW%d CR4/%d, %.1f MHz, %s (%s)\n", r.SF, r.BWHz, r.CodingRate+4,
		r.FrequencyMHz, r.Region, r.Regulatory)
	if r.appliesDutyCap() {
		fmt.Fprintf(b, "Duty cycle: %d%%, enforced, and applied to every node in the twin.\n",
			r.MaxDutyCyclePct)
	} else {
		fmt.Fprintf(b, "Duty cycle: %d%%, advisory, so the twin applies no regulatory cap.\n",
			r.MaxDutyCyclePct)
	}
	fmt.Fprintf(b, "\nNodes:\n\n")
	fmt.Fprintf(b, "  %-10s %-18s %-9s %-8s %s\n", "address", "name", "exported", "links",
		"firmware")
	for _, n := range g.Nodes {
		name := n.Name
		if name == "" {
			name = "-"
		}
		fw := n.Firmware
		if fw == "" {
			fw = "-"
		}
		fmt.Fprintf(b, "  %-10s %-18s %-9s %-8d %s\n", n.Address, name,
			yesNo(n.Exported), g.Degree(n.Address), fw)
	}
	fmt.Fprintf(b, "\n")
}

func writeTwinLinks(b *strings.Builder, g *twinGraph) {
	fmt.Fprintf(b, "Observed links\n")
	fmt.Fprintf(b, "--------------\n\n")
	fmt.Fprintf(b, "  %-10s %-10s %6s %5s  %s\n", "from", "to", "rssi", "snr", "source")
	for _, l := range g.Links {
		src := "observed"
		if !l.Observed {
			src = "assumed reciprocal"
		}
		fmt.Fprintf(b, "  %-10s %-10s %6d %5d  %s\n", l.From, l.To, l.RSSI, l.SNR, src)
	}
	fmt.Fprintf(b, "\n")
}

func writeTwinCoverage(b *strings.Builder, g *twinGraph) {
	fmt.Fprintf(b, "Reconstruction gaps\n")
	fmt.Fprintf(b, "-------------------\n\n")

	// Blocks are collected and then joined, so a section with three gaps and a
	// section with none are separated from what follows identically.
	var blocks []string

	if un := g.UnexportedNodes(); len(un) > 0 {
		blocks = append(blocks, fmt.Sprintf(
			"Nodes present only through other nodes' neighbour tables (%d): %s\n"+
				"  Export from these to replace one-sided evidence with measured links.",
			len(un), strings.Join(un, ", ")))
	}
	if ul := g.UnobservedLinks(); len(ul) > 0 {
		var sb strings.Builder
		fmt.Fprintf(&sb, "Link directions nobody reported, assumed reciprocal (%d):\n", len(ul))
		for _, l := range ul {
			fmt.Fprintf(&sb, "  %s -> %s at the reverse direction's %d dBm / %d dB\n", l.From, l.To,
				l.RSSI, l.SNR)
		}
		fmt.Fprintf(&sb, "  A real one-way link would make the mesh worse than the twin shows.")
		blocks = append(blocks, sb.String())
	}
	if ow := g.OneWayLinks(); len(ow) > 0 {
		var sb strings.Builder
		fmt.Fprintf(&sb, "One-way links, heard at one end and not the other (%d):\n", len(ow))
		for _, l := range ow {
			fmt.Fprintf(&sb, "  %s -> %s at %d dBm / %d dB, and %s does not report hearing %s\n",
				l.From, l.To, l.RSSI, l.SNR, l.From, l.To)
		}
		fmt.Fprintf(&sb, "  Both ends exported, so this asymmetry is measured, not assumed. No\n")
		fmt.Fprintf(&sb, "  protocol exchange crosses a one-way link, so the twin treats these two\n")
		fmt.Fprintf(&sb, "  ends as unconnected.")
		blocks = append(blocks, sb.String())
	}
	if len(g.RouteOnlyAddrs) > 0 {
		blocks = append(blocks, fmt.Sprintf(
			"Addresses seen in routing tables with no observed link (%d): %s\n"+
				"  These are real nodes of the real mesh. No export carries a link to\n"+
				"  them, so the twin leaves them out rather than inventing one, and the\n"+
				"  capacity figures below are for a smaller mesh than the one deployed.",
			len(g.RouteOnlyAddrs), strings.Join(g.RouteOnlyAddrs, ", ")))
	}
	for _, n := range g.Notes {
		blocks = append(blocks, "Note: "+n)
	}
	if len(blocks) == 0 {
		blocks = append(blocks,
			"None: every node exported, and every link was observed from both ends.")
	}
	fmt.Fprintf(b, "%s\n\n", strings.Join(blocks, "\n\n"))
}

func writeTwinCapacity(b *strings.Builder, p *twinCapacity) {
	fmt.Fprintf(b, "Capacity probe (simulation)\n")
	fmt.Fprintf(b, "---------------------------\n\n")
	fmt.Fprintf(b, "Offered load ramped over a %d s window, seed %d, one run per rate.\n",
		p.DurationMs/1000, p.Seed)
	fmt.Fprintf(b, "Traffic is the same construction the published scale runs use: one message\n")
	fmt.Fprintf(b, "at a time, round-robin source, destination half the fleet away. Delivery is\n")
	fmt.Fprintf(b, "message_delivery_rate (reached the destination); confirmed is\n")
	fmt.Fprintf(b, "confirmed_delivery_rate (the receipt made it back to the sender). Each row's\n")
	fmt.Fprintf(b, "percentages are over that row's message count, so the bottom of the ramp is\n")
	fmt.Fprintf(b, "the smallest sample on the table.\n\n")

	fmt.Fprintf(b, "  %10s %9s %9s %10s %9s %9s %9s %9s\n", "msgs/min", "messages", "delivered",
		"confirmed", "erlangs", "chan util", "control", "latency")
	for _, pt := range p.Points {
		fmt.Fprintf(b, "  %10g %9d %8.0f%% %9.0f%% %9.2f %8.1f%% %8.1f%% %8.0fms\n",
			pt.MsgsPerMin, pt.Scripted, pt.DeliveryRate*100, pt.ConfirmedRate*100,
			pt.OfferedLoadErlangs, pt.ChannelUtilPct, pt.ControlAirtimePct, pt.AvgLatencyMs)
	}
	fmt.Fprintf(b, "\n")

	switch {
	case len(p.Points) == 0:
		fmt.Fprintf(b, "No rates probed.\n\n")
	case p.BestDeliveryRate == 0:
		fmt.Fprintf(b, "Saturation knee: undefined. Not one message reached its destination at\n")
		fmt.Fprintf(b, "any probed rate, so there is no working rate to measure a knee against.\n")
		fmt.Fprintf(b, "Check the reconstruction gaps above before reading this as a capacity\n")
		fmt.Fprintf(b, "result: a mesh imported in disconnected pieces cannot deliver anything\n")
		fmt.Fprintf(b, "across them.\n\n")
	case p.SaturatedMsgsPerMin == 0:
		fmt.Fprintf(b, "Saturation knee: not reached. Delivery peaks at %.0f%% (%g msgs/min) and\n",
			p.BestDeliveryRate*100, p.PeakMsgsPerMin)
		fmt.Fprintf(b, "stays within %.0f%% of that all the way to %g msgs/min, the highest rate\n",
			twinKneeFraction*100, p.KneeMsgsPerMin)
		fmt.Fprintf(b, "probed. Ramp higher to find the knee.\n\n")
	default:
		fmt.Fprintf(b, "Saturation knee: %g msgs/min. Delivery peaks at %.0f%% (%g msgs/min) and\n",
			p.KneeMsgsPerMin, p.BestDeliveryRate*100, p.PeakMsgsPerMin)
		fmt.Fprintf(b, "holds within %.0f%% of that up to %g msgs/min, then falls away at %g\n",
			twinKneeFraction*100, p.KneeMsgsPerMin, p.SaturatedMsgsPerMin)
		fmt.Fprintf(b, "msgs/min.\n\n")
	}

	if len(p.BelowBarUnderPeak) > 0 {
		rates := make([]string, len(p.BelowBarUnderPeak))
		for i, r := range p.BelowBarUnderPeak {
			rates[i] = fmt.Sprintf("%g", r)
		}
		fmt.Fprintf(b, "Delivery is also below that bar at the BOTTOM of the ramp (%s msgs/min),\n",
			strings.Join(rates, ", "))
		fmt.Fprintf(b, "which is not saturation. A mesh carrying almost no traffic lets its routes\n")
		fmt.Fprintf(b, "expire between messages, so each message pays for a fresh discovery flood;\n")
		fmt.Fprintf(b, "those runs also carry the fewest messages, so they are the noisiest rows.\n\n")
	}
}

func writeTwinCriticality(b *strings.Builder, c *twinConnectivity) {
	fmt.Fprintf(b, "Node criticality (simulation)\n")
	fmt.Fprintf(b, "-----------------------------\n\n")

	if len(c.BaselineComponents) > 1 {
		fmt.Fprintf(b, "The imported mesh is ALREADY in %d disconnected pieces:\n",
			len(c.BaselineComponents))
		for i, grp := range c.BaselineComponents {
			fmt.Fprintf(b, "  piece %d (%d nodes): %s\n", i+1, len(grp), strings.Join(grp, ", "))
		}
		fmt.Fprintf(b, "Every row below is measured against that starting point: a node is\n")
		fmt.Fprintf(b, "counted as cut off only when it loses reach it had while the removed\n")
		fmt.Fprintf(b, "node was present, never for sitting in a piece it was already in.\n\n")
	} else {
		fmt.Fprintf(b, "With every node present the mesh is one connected piece.\n\n")
	}

	fmt.Fprintf(b, "Removing each node in turn:\n\n")
	fmt.Fprintf(b, "  %-10s %-18s %6s %7s  %s\n", "address", "name", "links", "pieces",
		"newly cut off")
	rows := make([]twinNodeCriticality, len(c.Nodes))
	copy(rows, c.Nodes)
	// Worst first: the nodes whose loss strands the most, then by degree, so
	// the top of the table is the list to go and add redundancy around.
	sort.SliceStable(rows, func(i, j int) bool {
		if len(rows[i].Isolated) != len(rows[j].Isolated) {
			return len(rows[i].Isolated) > len(rows[j].Isolated)
		}
		if rows[i].Degree != rows[j].Degree {
			return rows[i].Degree > rows[j].Degree
		}
		return rows[i].Address < rows[j].Address
	})
	critical := 0
	for _, r := range rows {
		name := r.Name
		if name == "" {
			name = "-"
		}
		cut := "nothing"
		if len(r.Isolated) > 0 {
			critical++
			cut = strings.Join(r.Isolated, ", ")
		}
		fmt.Fprintf(b, "  %-10s %-18s %6d %7d  %s\n", r.Address, name, r.Degree, r.Components, cut)
	}
	fmt.Fprintf(b, "\n")
	if critical == 0 {
		fmt.Fprintf(b, "No single node's loss partitions this mesh.\n\n")
	} else {
		fmt.Fprintf(b, "%d node(s) are single points of failure: losing any one of them strands\n",
			critical)
		fmt.Fprintf(b, "the nodes listed beside it. Adding a link that bypasses one is the\n")
		fmt.Fprintf(b, "cheapest resilience the twin can point at.\n\n")
	}
}

func yesNo(v bool) string {
	if v {
		return "yes"
	}
	return "no"
}
