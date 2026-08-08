package main

/*
#include <stdlib.h>
#include "bridge.h"
*/
import "C"

// Mesh digital twin: the two questions v1 answers about an imported mesh.
//
//   - Capacity. Ramp the offered message rate over the reconstructed graph and
//     watch end-to-end delivery fall, which is where a deployment's usable
//     message rate actually is.
//   - Criticality. Remove each node in turn and see what the mesh breaks into,
//     which is where its single points of failure are.
//
// Both run the real protocol code: the capacity probe drives whole scenarios
// through the normal headless runner, and the criticality sweep asks the same
// anomaly_partition_components traversal the shipped mesh_partition detector
// uses (docs/bramble-anomaly-detection.md).

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"unsafe"
)

// twinMaxNodes is how many nodes a scenario may hold (MAX_NODES in
// simulator/engine/sim_node.h). An import larger than this is refused with the
// count rather than silently truncated to a mesh the operator did not ask
// about.
const twinMaxNodes = C.MAX_NODES

// ── Node criticality ─────────────────────────────────────────────────────

// twinNodeCriticality is what removing one node does to the mesh.
type twinNodeCriticality struct {
	Address string `json:"address"`
	Name    string `json:"name,omitempty"`
	// Degree is how many nodes this one reaches directly.
	Degree int `json:"degree"`
	// Components is how many connected pieces the mesh falls into once this
	// node is gone (its own removal is not counted as a piece).
	Components int `json:"components"`
	// Isolated lists every remaining node that this removal newly strands: a
	// node that could reach the rest of its own baseline piece with this node
	// present and cannot without it. Measured against the baseline, so a mesh
	// imported in several pieces does not report every node as a cut node
	// merely for sitting outside the biggest piece. Empty means removing this
	// node costs reach but not connectivity.
	Isolated []string `json:"isolated,omitempty"`
}

// twinConnectivity is the reconstructed mesh's connectivity before anything is
// removed, plus the per-node removal sweep.
type twinConnectivity struct {
	// Components with every node present. More than one means the imported
	// mesh is already partitioned, which every criticality row has to be read
	// against.
	BaselineComponents [][]string            `json:"baseline_components"`
	Nodes              []twinNodeCriticality `json:"nodes"`
}

// twinTopology is a loaded scenario's node array and radio config, without the
// event loop: enough to ask connectivity questions and nothing more.
type twinTopology struct {
	nodes  *C.node_array_t
	radio  *C.radio_config_t
	events *C.event_queue_t
	rng    *C.pcg32_state_t
	ids    []string
}

func loadTwinTopology(path string) (*twinTopology, error) {
	t := &twinTopology{
		nodes:  (*C.node_array_t)(C.calloc(1, C.sizeof_node_array_t)),
		radio:  (*C.radio_config_t)(C.calloc(1, C.sizeof_radio_config_t)),
		events: (*C.event_queue_t)(C.calloc(1, C.sizeof_event_queue_t)),
		rng:    (*C.pcg32_state_t)(C.calloc(1, C.sizeof_pcg32_state_t)),
	}
	C.node_array_init(t.nodes)
	C.radio_config_init(t.radio)
	C.event_queue_init(t.events)
	if _, ok := loadScenario(path, t.nodes, t.radio, t.events, t.rng); !ok {
		t.free()
		return nil, fmt.Errorf("scenario %s failed to load", path)
	}
	for i := 0; i < int(t.nodes.count); i++ {
		t.ids = append(t.ids, C.GoString(&t.nodes.nodes[i].id[0]))
	}
	return t, nil
}

func (t *twinTopology) free() {
	C.free(unsafe.Pointer(t.nodes))
	C.free(unsafe.Pointer(t.radio))
	C.free(unsafe.Pointer(t.events))
	C.free(unsafe.Pointer(t.rng))
}

// groups runs the partition traversal over whatever is currently active and
// returns the node ids of each connected component, largest first (ties broken
// by the lowest member id, so the ordering is stable).
func (t *twinTopology) groups() [][]string {
	comp, count := partitionComponents(t.nodes, t.radio)
	groups := make([][]string, count)
	for i, c := range comp {
		if c < 0 {
			continue
		}
		groups[c] = append(groups[c], t.ids[i])
	}
	for _, g := range groups {
		sort.Strings(g)
	}
	sort.Slice(groups, func(i, j int) bool {
		if len(groups[i]) != len(groups[j]) {
			return len(groups[i]) > len(groups[j])
		}
		return groups[i][0] < groups[j][0]
	})
	return groups
}

// setActive flips one node's active flag directly. node_deactivate would also
// wipe the node's protocol state, which is exactly what a real node_leave
// means but is wrong for a hypothetical sweep that has to put every node back
// afterwards. Connectivity reads nothing but this flag.
func (t *twinTopology) setActive(i int, active bool) {
	C.node_array_get(t.nodes, C.int(i)).active = C.bool(active)
}

// twinAnalyzeConnectivity loads the twin scenario and removes each node in
// turn, reporting what the mesh falls apart into.
func twinAnalyzeConnectivity(scenarioPath string, g *twinGraph) (*twinConnectivity, error) {
	t, err := loadTwinTopology(scenarioPath)
	if err != nil {
		return nil, err
	}
	defer t.free()

	baseline := t.groups()
	out := &twinConnectivity{BaselineComponents: baseline}
	baseOf := map[string]int{}
	for gi, grp := range baseline {
		for _, id := range grp {
			baseOf[id] = gi
		}
	}

	for i, id := range t.ids {
		t.setActive(i, false)
		groups := t.groups()
		t.setActive(i, true)

		row := twinNodeCriticality{
			Address:    id,
			Degree:     g.Degree(id),
			Components: len(groups),
			Isolated:   twinNewlyStranded(baseOf, groups),
		}
		if n := g.NodeByAddress(id); n != nil {
			row.Name = n.Name
		}
		out.Nodes = append(out.Nodes, row)
	}
	return out, nil
}

// twinNewlyStranded reports which nodes a removal actually cut off, given the
// baseline component every node started in and the components that remain.
//
// Deactivating one node can only split a component, never merge two, so every
// surviving component sits entirely inside one baseline component. A baseline
// piece that survives as a single component lost nothing; one that survives as
// several lost everything outside its largest surviving piece. Anything that
// was already in a different baseline piece is not stranded by this removal: it
// was never reachable to begin with, and counting it is what turns an
// already-partitioned import into a table where every node reads as a single
// point of failure.
//
// groups arrives largest-piece-first (twinTopology.groups), so the first
// surviving piece seen for a baseline component is that component's largest.
func twinNewlyStranded(baseOf map[string]int, groups [][]string) []string {
	kept := map[int]bool{}
	var out []string
	for _, grp := range groups {
		if len(grp) == 0 {
			continue
		}
		base := baseOf[grp[0]]
		if !kept[base] {
			kept[base] = true
			continue
		}
		out = append(out, grp...)
	}
	sort.Strings(out)
	return out
}

// ── Capacity probe ───────────────────────────────────────────────────────

// twinCapacityPoint is one offered-load point of the ramp.
type twinCapacityPoint struct {
	MsgsPerMin  float64 `json:"msgs_per_min"`
	Scripted    int     `json:"scripted_messages"`
	Delivered   uint64  `json:"delivered"`
	Dropped     uint64  `json:"dropped"`
	Undelivered uint64  `json:"undelivered"`
	Confirmed   uint64  `json:"confirmed"`

	DeliveryRate       float64 `json:"delivery_rate"`
	ConfirmedRate      float64 `json:"confirmed_delivery_rate"`
	OfferedLoadErlangs float64 `json:"offered_load_erlangs"`
	ChannelUtilPct     float64 `json:"channel_util_pct"`
	ControlAirtimePct  float64 `json:"control_airtime_pct"`
	AvgLatencyMs       float64 `json:"avg_latency_ms"`
}

// twinCapacity is the whole ramp plus where delivery gave way.
type twinCapacity struct {
	DurationMs int64               `json:"duration_ms"`
	Seed       uint64              `json:"seed"`
	Points     []twinCapacityPoint `json:"points"`

	// BestDeliveryRate is the highest delivery rate anywhere on the ramp, and
	// PeakMsgsPerMin the offered rate that achieved it. The knee is measured
	// from there, because a ramp is not guaranteed to start at its best: a
	// mesh carrying very little traffic can deliver worse than a busier one,
	// since routes go stale between messages and each one pays for a fresh
	// discovery.
	BestDeliveryRate float64 `json:"best_delivery_rate"`
	PeakMsgsPerMin   float64 `json:"peak_msgs_per_min"`
	// KneeMsgsPerMin is the highest offered rate from the peak upwards that
	// still held delivery at or above twinKneeFraction of BestDeliveryRate,
	// with every rate between the peak and it holding too. Never below the
	// peak rate, which holds the bar by construction.
	KneeMsgsPerMin float64 `json:"knee_msgs_per_min"`
	// SaturatedMsgsPerMin is the first rate above the knee that fell below the
	// bar. Zero means the ramp never fell below it, so the knee is at or above
	// the highest rate probed and the honest answer is "not found in this
	// range".
	SaturatedMsgsPerMin float64 `json:"saturated_msgs_per_min"`
	// BelowBarUnderPeak lists rates below the peak that also failed the bar,
	// i.e. the bottom of the ramp delivering worse than the middle of it. Not
	// saturation, and the report says so rather than letting it read as one.
	BelowBarUnderPeak []float64 `json:"below_bar_under_peak,omitempty"`
}

// twinKneeFraction is how far delivery may fall from the ramp's best before
// the offered rate counts as past the knee. Nine tenths: far enough to ignore
// the odd message lost to a collision, tight enough that the collapse
// documented in docs/results/simulation-2026-07-honest-baseline.md (95% to 12%
// between 10 and 50 nodes) is caught at its first real step.
const twinKneeFraction = 0.9

// twinRunCapacityProbe ramps the offered message rate over the reconstructed
// graph, one full scenario run per rate, and reports the curve.
//
// workDir holds the generated scenario files; the caller owns it.
func twinRunCapacityProbe(g *twinGraph, workDir string, seed uint64, durationMs int64,
	rates []float64) (*twinCapacity, error) {
	probe := &twinCapacity{DurationMs: durationMs, Seed: seed}
	addrs := g.Addresses()

	for _, rate := range rates {
		events := twinTrafficEvents(addrs, durationMs, rate)
		sc := buildTwinScenario(g, twinScenarioName("twin-capacity", rate), seed, durationMs, events)
		path := filepath.Join(workDir, fmt.Sprintf("capacity-%g.json", rate))
		data, err := sc.JSON()
		if err != nil {
			return nil, err
		}
		if err := os.WriteFile(path, data, 0o644); err != nil {
			return nil, err
		}

		metrics, err := runScenarioFinalMetrics(path)
		if err != nil {
			return nil, fmt.Errorf("capacity probe at %g msgs/min: %w", rate, err)
		}

		pt := twinCapacityPoint{
			MsgsPerMin:         rate,
			Scripted:           len(events),
			Delivered:          metrics.uint("delivered"),
			Dropped:            metrics.uint("dropped"),
			Undelivered:        metrics.uint("undelivered"),
			Confirmed:          metrics.uint("confirmed"),
			DeliveryRate:       metrics.float("message_delivery_rate"),
			ConfirmedRate:      metrics.float("confirmed_delivery_rate"),
			OfferedLoadErlangs: metrics.float("offered_load_erlangs"),
			ChannelUtilPct:     metrics.float("channel_util_pct"),
			ControlAirtimePct:  metrics.float("control_airtime_pct"),
			AvgLatencyMs:       metrics.float("avg_latency_ms"),
		}
		probe.Points = append(probe.Points, pt)
	}

	probe.findKnee()
	return probe, nil
}

// findKnee fills in the saturation figures from the measured ramp.
//
// The knee is looked for above the ramp's best point rather than from the
// bottom up. A delivery-against-offered-load curve is not required to be
// monotonic: a mesh offered very little traffic can deliver worse than a busier
// one, because routes expire between messages and each message then pays for a
// fresh discovery flood. Scanning from the bottom would report that as
// saturation at the lowest rate probed, which is the opposite of what it is.
func (c *twinCapacity) findKnee() {
	peak := -1
	for i, p := range c.Points {
		if peak < 0 || p.DeliveryRate > c.BestDeliveryRate {
			peak = i
			c.BestDeliveryRate = p.DeliveryRate
		}
	}
	if peak < 0 {
		return
	}
	c.PeakMsgsPerMin = c.Points[peak].MsgsPerMin
	bar := c.BestDeliveryRate * twinKneeFraction

	c.KneeMsgsPerMin = c.Points[peak].MsgsPerMin
	for i := peak + 1; i < len(c.Points); i++ {
		if c.Points[i].DeliveryRate < bar {
			c.SaturatedMsgsPerMin = c.Points[i].MsgsPerMin
			break
		}
		c.KneeMsgsPerMin = c.Points[i].MsgsPerMin
	}
	for i := 0; i < peak; i++ {
		if c.Points[i].DeliveryRate < bar {
			c.BelowBarUnderPeak = append(c.BelowBarUnderPeak, c.Points[i].MsgsPerMin)
		}
	}
}

// ── Running a scenario and reading its terminal metrics ──────────────────

// twinMetrics is a decoded final_metrics event.
type twinMetrics map[string]any

func (m twinMetrics) float(key string) float64 {
	v, _ := m[key].(float64)
	return v
}

func (m twinMetrics) uint(key string) uint64 {
	v, _ := m[key].(float64)
	if v < 0 {
		return 0
	}
	return uint64(v)
}

// runScenarioFinalMetrics runs a scenario headlessly and returns its
// final_metrics event. That event is emitted Go-side (sim.complete), straight
// to the broadcast callback, so it does not ride the C-stdout pipe and cannot
// be dropped the way the C-emitted per-packet lines can.
func runScenarioFinalMetrics(path string) (twinMetrics, error) {
	result, err := runScenarioQuiet(path)
	if err != nil {
		return nil, err
	}
	for _, line := range result.Lines() {
		if !strings.Contains(line, `"final_metrics"`) {
			continue
		}
		var evt map[string]any
		if json.Unmarshal([]byte(line), &evt) != nil {
			continue
		}
		if evt["type"] == "final_metrics" {
			return twinMetrics(evt), nil
		}
	}
	return nil, fmt.Errorf("%s: run emitted no final_metrics event", path)
}
