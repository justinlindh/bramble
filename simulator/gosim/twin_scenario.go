package main

// Mesh digital twin: turning a merged link graph into a runnable scenario.
//
// The output is an ordinary gosim scenario file (simulator/engine/sim_scenario.c),
// so a twin runs through exactly the same protocol code, airtime accounting and
// collision model as every other scenario. The one thing that differs is where
// reachability comes from: the "links" block puts the radio in link mode, and
// node coordinates stop meaning anything.

import (
	"encoding/json"
	"fmt"
	"math"
)

// twinScenarioDurationMs is the observation window every twin analysis runs
// over: ten minutes, the same window docs/results/simulation-2026-07-honest-baseline.md
// measures its scale numbers in, so a twin's offered-load and delivery figures
// are read on the same scale as the published baseline.
const twinScenarioDurationMs = 600000

// twinDisplayRadius is the radius, in grid units, of the ring the twin lays its
// nodes out on. Purely cosmetic: link mode never reads a coordinate, and there
// is nothing in an export to derive a real position from. It exists so the
// simulator UI draws a legible circle instead of stacking every node on the
// origin.
const twinDisplayRadius = 200.0

type twinScenarioNode struct {
	ID string  `json:"id"`
	X  float64 `json:"x"`
	Y  float64 `json:"y"`
}

type twinScenarioLink struct {
	From string `json:"from"`
	To   string `json:"to"`
	RSSI int    `json:"rssi"`
	SNR  int    `json:"snr"`
}

type twinScenarioRadio struct {
	SF           int  `json:"sf"`
	BWHz         int  `json:"bw_hz"`
	CR           int  `json:"cr"`
	TxPowerDBm   int  `json:"tx_power_dbm"`
	DutyCyclePct *int `json:"duty_cycle_pct,omitempty"`
}

type twinScenarioEvent struct {
	AtMs int64  `json:"at_ms"`
	Type string `json:"type"`
	Src  string `json:"src"`
	Dest string `json:"dest"`
}

type twinScenario struct {
	Name       string              `json:"name"`
	Mode       string              `json:"mode"`
	DurationMs int64               `json:"duration_ms"`
	Seed       uint64              `json:"seed"`
	Radio      twinScenarioRadio   `json:"radio"`
	Nodes      []twinScenarioNode  `json:"nodes"`
	Links      []twinScenarioLink  `json:"links"`
	Events     []twinScenarioEvent `json:"events"`
}

// buildTwinScenario renders the graph as a scenario. events is the scripted
// traffic to run over it, empty for a topology-only scenario.
func buildTwinScenario(g *twinGraph, name string, seed uint64, durationMs int64,
	events []twinScenarioEvent) *twinScenario {
	sc := &twinScenario{
		Name:       name,
		Mode:       "deterministic",
		DurationMs: durationMs,
		Seed:       seed,
		Radio: twinScenarioRadio{
			SF:         g.Radio.SF,
			BWHz:       g.Radio.BWHz,
			CR:         g.Radio.CodingRate,
			TxPowerDBm: g.Radio.TxPowerDBm,
		},
		Events: events,
	}
	// A plan that hard-enforces a duty cycle (EU868's 1%) is a real ceiling on
	// what the deployment may transmit, so the twin applies it through the
	// firmware's own airtime_budget_set_duty_cap. An advisory 100% ceiling is
	// no ceiling and is left off, which is the sim's unlimited default.
	if g.Radio.DutyCycleEnforced && g.Radio.MaxDutyCyclePct > 0 && g.Radio.MaxDutyCyclePct < 100 {
		pct := g.Radio.MaxDutyCyclePct
		sc.Radio.DutyCyclePct = &pct
	}

	n := len(g.Nodes)
	for i, node := range g.Nodes {
		angle := 2.0 * math.Pi * float64(i) / float64(n)
		sc.Nodes = append(sc.Nodes, twinScenarioNode{
			ID: node.Address,
			X:  round2(twinDisplayRadius * math.Cos(angle)),
			Y:  round2(twinDisplayRadius * math.Sin(angle)),
		})
	}
	for _, l := range g.Links {
		sc.Links = append(sc.Links, twinScenarioLink{
			From: l.From,
			To:   l.To,
			RSSI: l.RSSI,
			SNR:  l.SNR,
		})
	}
	if sc.Events == nil {
		sc.Events = []twinScenarioEvent{}
	}
	return sc
}

func round2(v float64) float64 { return math.Round(v*100) / 100 }

// JSON renders the scenario file.
func (s *twinScenario) JSON() ([]byte, error) {
	b, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		return nil, err
	}
	return append(b, '\n'), nil
}

// twinTrafficEvents scripts the offered load for one point of the capacity
// probe.
//
// Deliberately the same construction simulator/scenarios/generate.py uses for
// the published scale runs: one message every 60000/msgsPerMin ms, starting at
// 10 s and stopping 10 s before the window ends, with a round-robin source and
// a destination half the fleet away. Reusing the method is what makes a twin's
// delivery-against-offered-load curve readable on the same terms as
// docs/results/simulation-2026-07-honest-baseline.md rather than as a number
// with its own private definition.
func twinTrafficEvents(addrs []string, durationMs int64, msgsPerMin float64) []twinScenarioEvent {
	var events []twinScenarioEvent
	if msgsPerMin <= 0 || len(addrs) < 2 {
		return events
	}
	interval := 60000.0 / msgsPerMin
	count := len(addrs)
	// The generator's "half the fleet away" offset, count/2+1, is a full lap at
	// two nodes: it would name the sender as its own destination and the
	// generator's own src != dest guard would then script no message at all. A
	// two-node mesh has exactly one possible destination, so use it. Every
	// larger fleet keeps the published offset unchanged.
	offset := count/2 + 1
	if count == 2 {
		offset = 1
	}
	timeMs := 10000.0
	msgID := 0
	for timeMs < float64(durationMs)-10000.0 {
		src := msgID % count
		dest := (msgID + offset) % count
		if src != dest {
			events = append(events, twinScenarioEvent{
				AtMs: int64(timeMs),
				Type: "send_message",
				Src:  addrs[src],
				Dest: addrs[dest],
			})
		}
		timeMs += interval
		msgID++
	}
	return events
}

// twinScenarioName builds a stable scenario name for a probe point.
func twinScenarioName(prefix string, msgsPerMin float64) string {
	return fmt.Sprintf("%s-%gmsgmin", prefix, msgsPerMin)
}
