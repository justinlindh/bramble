package main

// Mesh digital twin: the `twin` subcommand's own plumbing.
//
// The analyses are covered elsewhere; what is checked here is the contract the
// command makes with whatever reads it. A machine-readable mode that emits
// anything but one JSON document on stdout is unusable from a pipeline, and it
// is exactly the sort of break no unit test of the analyses would ever see.

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// twinFixturePaths is the committed example export set, the same files
// docs/digital-twin.md works through.
func twinFixturePaths() []string {
	var out []string
	for _, name := range []string{"basecamp", "creek", "ridge", "tower"} {
		out = append(out, filepath.Join("testdata", "twin", name+".json"))
	}
	return out
}

func TestTwinJSONToStdoutEmitsOnlyJSON(t *testing.T) {
	var out, errw bytes.Buffer
	args := append([]string{"-skip-capacity", "-json", "-"}, twinFixturePaths()...)
	if code := runTwinIO(args, &out, &errw); code != 0 {
		t.Fatalf("twin exited %d\n%s", code, errw.String())
	}

	var payload twinReportJSON
	if err := json.Unmarshal(out.Bytes(), &payload); err != nil {
		t.Fatalf("stdout is not one JSON document: %v\n%s", err, out.String())
	}
	if len(payload.Nodes) != 5 || len(payload.Links) != 8 {
		t.Fatalf("payload has %d nodes and %d links", len(payload.Nodes), len(payload.Links))
	}
	if payload.Connectivity == nil || len(payload.Connectivity.Nodes) != 5 {
		t.Fatalf("payload carries no criticality sweep: %+v", payload.Connectivity)
	}
	if payload.Capacity != nil {
		t.Fatalf("-skip-capacity still reported a capacity probe")
	}
	if payload.Assumptions.ReciprocalLinks != 1 || payload.Assumptions.UnexportedNodes != 1 {
		t.Fatalf("payload assumptions %+v", payload.Assumptions)
	}
	// The human report is not lost, it moves to stderr.
	if !strings.Contains(errw.String(), "Bramble mesh digital twin") {
		t.Fatalf("the text report went nowhere:\n%s", errw.String())
	}
}

func TestTwinJSONToAFileKeepsTheReportOnStdout(t *testing.T) {
	path := filepath.Join(t.TempDir(), "twin.json")
	scenarioPath := filepath.Join(t.TempDir(), "scenario.json")
	var out, errw bytes.Buffer
	args := append([]string{"-skip-capacity", "-json", path, "-scenario", scenarioPath},
		twinFixturePaths()...)
	if code := runTwinIO(args, &out, &errw); code != 0 {
		t.Fatalf("twin exited %d\n%s", code, errw.String())
	}

	if !strings.Contains(out.String(), "Bramble mesh digital twin") {
		t.Fatalf("stdout does not carry the report:\n%s", out.String())
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read -json output: %v", err)
	}
	var payload twinReportJSON
	if err := json.Unmarshal(data, &payload); err != nil {
		t.Fatalf("-json output is not JSON: %v", err)
	}
	if payload.Scenario == nil || len(payload.Scenario.Nodes) != 5 {
		t.Fatalf("payload carries no runnable scenario")
	}
	// -scenario writes the same topology-only scenario as a standalone file.
	scenario, err := os.ReadFile(scenarioPath)
	if err != nil {
		t.Fatalf("read -scenario output: %v", err)
	}
	if _, err := loadTwinTopology(scenarioPath); err != nil {
		t.Fatalf("the written scenario does not load: %v\n%s", err, scenario)
	}
}

func TestTwinRefusesAnUnusableInvocation(t *testing.T) {
	cases := []struct {
		name string
		args []string
		want string
	}{
		{name: "no exports", args: nil, want: "usage:"},
		{
			name: "a descending ramp",
			args: append([]string{"-rates", "5,1"}, twinFixturePaths()...),
			want: "rates must ascend",
		},
		{
			name: "a window with no room for traffic",
			args: append([]string{"-duration-ms", "5000"}, twinFixturePaths()...),
			want: "shorter than the 20 s",
		},
		{
			name: "a file that is not an export",
			args: []string{"-skip-capacity", filepath.Join("testdata", "twin")},
			want: "twin:",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var out, errw bytes.Buffer
			if code := runTwinIO(tc.args, &out, &errw); code == 0 {
				t.Fatalf("twin accepted %v", tc.args)
			}
			if !strings.Contains(errw.String(), tc.want) {
				t.Fatalf("stderr does not explain the refusal (want %q):\n%s",
					tc.want, errw.String())
			}
			if out.Len() != 0 {
				t.Fatalf("a refused run still wrote to stdout:\n%s", out.String())
			}
		})
	}
}
