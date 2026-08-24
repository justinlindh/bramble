package main

// Mesh digital twin: the `twin` subcommand.
//
// Collect one bramble.exportTopology document per node from a running
// deployment, hand the files to this command, and it reconstructs the mesh as a
// runnable scenario and answers two questions about it: how much message rate
// the mesh carries before delivery falls away, and which nodes are single
// points of failure. Both answers come from running the real protocol code over
// the reconstruction, not from a formula.
//
// What the reconstruction is and is not is spelled out in
// ../../docs/digital-twin.md and restated at the top of every report: it
// replays the links those nodes reported at the moment of export, and predicts
// nothing about propagation or about nodes that are not deployed.

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// twinDefaultRates is the offered-load ramp the capacity probe walks when the
// operator does not name one: a decade and a half of message rate, coarse
// enough to run in a handful of scenario passes and fine enough to bracket the
// knee for the mesh sizes a twin is built from.
const twinDefaultRates = "1,2,5,10,20,40"

// twinReportJSON is the machine-readable form of a run, written by -json. It
// carries the same numbers the text report shows, so a dashboard and a human
// are never reading two different analyses.
type twinReportJSON struct {
	Sources        []string              `json:"sources"`
	Nodes          []twinNode            `json:"nodes"`
	Links          []twinLink            `json:"links"`
	Radio          twinExportRadio       `json:"radio"`
	RouteOnlyAddrs []string              `json:"route_only_addresses,omitempty"`
	Notes          []string              `json:"notes,omitempty"`
	Connectivity   *twinConnectivity     `json:"connectivity,omitempty"`
	Capacity       *twinCapacity         `json:"capacity,omitempty"`
	Scenario       *twinScenario         `json:"scenario,omitempty"`
	Assumptions    twinReportAssumptions `json:"assumptions"`
}

// twinReportAssumptions restates, in the machine-readable output, the bounds
// the text report states in prose. A consumer that only ever reads the JSON
// still has to be told these numbers are simulation over an observed snapshot.
type twinReportAssumptions struct {
	Kind                string `json:"kind"`
	ReciprocalLinks     int    `json:"reciprocal_links_assumed"`
	OneWayLinks         int    `json:"one_way_links"`
	UnexportedNodes     int    `json:"unexported_nodes"`
	ObservationWindowMs int64  `json:"observation_window_ms"`
}

func runTwin(args []string) int {
	return runTwinIO(args, os.Stdout, os.Stderr)
}

// runTwinIO is runTwin with its two output streams named, which is what lets a
// test read both and check that "-json -" really produces a document a consumer
// can parse.
func runTwinIO(args []string, out, errw io.Writer) int {
	fs := flag.NewFlagSet("twin", flag.ContinueOnError)
	fs.SetOutput(errw)
	fs.Usage = func() {
		fmt.Fprintln(errw,
			"usage: bramble-gosim twin [flags] <export.json> [export.json ...]")
		fmt.Fprintln(errw,
			"\nEach export.json is one node's bramble.exportTopology result (the bare")
		fmt.Fprintln(errw,
			"result object or the whole JSON-RPC response). Flags:")
		fs.PrintDefaults()
	}
	rates := fs.String("rates", twinDefaultRates,
		"comma-separated offered message rates (messages per minute) for the capacity probe")
	seed := fs.Uint64("seed", 1, "PRNG seed for every scenario the probe runs")
	durationMs := fs.Int64("duration-ms", twinScenarioDurationMs,
		"observation window of each capacity run, in milliseconds")
	skipCapacity := fs.Bool("skip-capacity", false,
		"skip the capacity probe (which runs one full scenario per rate) and report topology "+
			"and criticality only")
	jsonOut := fs.String("json", "",
		"also write the machine-readable report to this path (\"-\" for stdout, which moves "+
			"the human report to stderr so stdout is one JSON document)")
	scenarioOut := fs.String("scenario", "",
		"write the reconstructed gosim scenario to this path, to run or edit by hand")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	sources := fs.Args()
	if len(sources) == 0 {
		fs.Usage()
		return 2
	}

	rateList, err := parseTwinRates(*rates)
	if err != nil {
		fmt.Fprintf(errw, "twin: %v\n", err)
		return 2
	}
	if *durationMs <= 20000 {
		fmt.Fprintf(errw,
			"twin: -duration-ms %d is shorter than the 20 s of ramp-up and drain every probe "+
				"run reserves, so no traffic would be offered\n", *durationMs)
		return 2
	}

	exports := make([]*twinExport, 0, len(sources))
	for _, path := range sources {
		exp, err := loadTwinExport(path)
		if err != nil {
			fmt.Fprintf(errw, "twin: %v\n", err)
			return 1
		}
		exports = append(exports, exp)
	}

	graph, err := buildTwinGraph(exports)
	if err != nil {
		fmt.Fprintf(errw, "twin: %v\n", err)
		return 1
	}
	if len(graph.Nodes) > twinMaxNodes {
		fmt.Fprintf(errw,
			"twin: %d nodes, more than the simulator's %d-node ceiling (MAX_NODES in "+
				"simulator/engine/sim_node.h)\n", len(graph.Nodes), twinMaxNodes)
		return 1
	}

	workDir, err := os.MkdirTemp("", "bramble-twin-")
	if err != nil {
		fmt.Fprintf(errw, "twin: %v\n", err)
		return 1
	}
	defer os.RemoveAll(workDir)

	// The topology-only scenario: the reconstruction with no scripted traffic.
	// It is what the criticality sweep loads, and what -scenario writes out.
	topo := buildTwinScenario(graph, "twin-topology", *seed, *durationMs, nil)
	topoData, err := topo.JSON()
	if err != nil {
		fmt.Fprintf(errw, "twin: %v\n", err)
		return 1
	}
	topoPath := filepath.Join(workDir, "topology.json")
	if err := os.WriteFile(topoPath, topoData, 0o644); err != nil {
		fmt.Fprintf(errw, "twin: %v\n", err)
		return 1
	}
	if *scenarioOut != "" {
		if err := os.WriteFile(*scenarioOut, topoData, 0o644); err != nil {
			fmt.Fprintf(errw, "twin: %v\n", err)
			return 1
		}
	}

	conn, err := twinAnalyzeConnectivity(topoPath, graph)
	if err != nil {
		fmt.Fprintf(errw, "twin: %v\n", err)
		return 1
	}

	var probe *twinCapacity
	if !*skipCapacity {
		probe, err = twinRunCapacityProbe(graph, workDir, *seed, *durationMs, rateList)
		if err != nil {
			fmt.Fprintf(errw, "twin: %v\n", err)
			return 1
		}
	}

	// With the JSON going to stdout, stdout has to be exactly one JSON
	// document: a consumer piping into jq gets a parse error otherwise. The
	// human report still gets written, to stderr, so nothing is lost.
	textOut := out
	if *jsonOut == "-" {
		textOut = errw
	}
	fmt.Fprint(textOut, twinReport(graph, conn, probe, sources))

	if *jsonOut != "" {
		payload := twinReportJSON{
			Sources:        sources,
			Nodes:          graph.Nodes,
			Links:          graph.Links,
			Radio:          graph.Radio,
			RouteOnlyAddrs: graph.RouteOnlyAddrs,
			Notes:          graph.Notes,
			Connectivity:   conn,
			Capacity:       probe,
			Scenario:       topo,
			Assumptions: twinReportAssumptions{
				Kind: "simulation over an observed link snapshot; not a field measurement " +
					"and not a propagation prediction",
				ReciprocalLinks:     len(graph.UnobservedLinks()),
				OneWayLinks:         len(graph.OneWayLinks()),
				UnexportedNodes:     len(graph.UnexportedNodes()),
				ObservationWindowMs: *durationMs,
			},
		}
		data, err := json.MarshalIndent(payload, "", "  ")
		if err != nil {
			fmt.Fprintf(errw, "twin: %v\n", err)
			return 1
		}
		data = append(data, '\n')
		if *jsonOut == "-" {
			if _, err := out.Write(data); err != nil {
				fmt.Fprintf(errw, "twin: %v\n", err)
				return 1
			}
		} else if err := os.WriteFile(*jsonOut, data, 0o644); err != nil {
			fmt.Fprintf(errw, "twin: %v\n", err)
			return 1
		}
	}
	return 0
}

// parseTwinRates reads the -rates list into an ascending ramp. The ramp has to
// ascend for the knee to mean anything: findKnee reports the last rate that
// held delivery before the first that did not, which is only a knee if the
// rates were probed in increasing order.
func parseTwinRates(s string) ([]float64, error) {
	var out []float64
	for field := range strings.SplitSeq(s, ",") {
		field = strings.TrimSpace(field)
		if field == "" {
			continue
		}
		v, err := strconv.ParseFloat(field, 64)
		if err != nil {
			return nil, fmt.Errorf("rate %q is not a number", field)
		}
		if v <= 0 {
			return nil, fmt.Errorf("rate %g must be positive", v)
		}
		if len(out) > 0 && v <= out[len(out)-1] {
			return nil, fmt.Errorf("rates must ascend: %g does not follow %g", v, out[len(out)-1])
		}
		out = append(out, v)
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no rates given")
	}
	return out, nil
}
