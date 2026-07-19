package main

// Scenario event-type gate (issue #166, same family as #144).
//
// simulator/engine/sim_scenario.c used to skip any event whose "type" it did
// not recognize, printing a warning nobody read. Three checked-in scenarios
// spelled their movement event "node_move" while the engine parsed only
// "move_node", so their movement phases never ran and every run still looked
// healthy. That is the same failure mode as #144: a scenario that quietly
// tests less than its name claims.
//
// The engine now refuses to load a scenario containing an event type it cannot
// execute. These tests keep that guarantee honest from the Go side: the first
// proves every checked-in scenario still parses (so the hard failure cannot be
// introduced by a stale spelling without CI noticing), and the second proves
// the movement phase of reliability-path-trace actually reaches the simulation
// rather than being parsed and dropped.

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// scenarioEventTypes returns every distinct event "type" string declared by
// the scenario file at path.
func scenarioEventTypes(t *testing.T, path string) []string {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	var doc struct {
		Events []struct {
			Type string `json:"type"`
		} `json:"events"`
	}
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatalf("parse %s: %v", path, err)
	}
	seen := map[string]bool{}
	var out []string
	for _, e := range doc.Events {
		if e.Type != "" && !seen[e.Type] {
			seen[e.Type] = true
			out = append(out, e.Type)
		}
	}
	return out
}

// engineEventTypes is the set of event type spellings the engine can execute,
// mirroring the branches in load_events (simulator/engine/sim_scenario.c).
// A type outside this set makes scenario_load_file fail, so a scenario using
// one is a build-time bug, not a runtime warning.
var engineEventTypes = map[string]bool{
	"send_message":     true,
	"generate_message": true,
	"send_attestation": true,
	"provision_anchor": true,
	"move_node":        true,
	"kill_node":        true,
	"node_leave":       true,
	"interference":     true,
	"join":             true,
	"node_join":        true,
}

// engineKnownUnimplementedEventTypes mirrors event_type_is_known_unimplemented
// in sim_scenario.c: recognized intent with no engine implementation. These
// load with a loud warning instead of failing, so they are legal in a scenario
// file but must never grow silently.
var engineKnownUnimplementedEventTypes = map[string]bool{
	"send_location": true,
}

// TestScenarioEventTypesAreExecutable asserts that no checked-in scenario
// declares an event type the engine would reject. Before #166 a typo here cost
// nothing at load time and silently removed a whole phase of the scenario;
// now it fails the load, so this test is the early warning that names the file
// and the offending spelling instead of leaving a maintainer to debug an
// engine error message.
func TestScenarioEventTypesAreExecutable(t *testing.T) {
	paths, err := filepath.Glob("../scenarios/*.json")
	if err != nil {
		t.Fatalf("glob scenarios: %v", err)
	}
	if len(paths) == 0 {
		t.Fatal("no scenario files found; the glob or the scenarios directory moved")
	}
	for _, path := range paths {
		for _, ty := range scenarioEventTypes(t, path) {
			switch {
			case engineEventTypes[ty]:
				// executable
			case engineKnownUnimplementedEventTypes[ty]:
				t.Logf("%s declares %q, which the engine recognizes but cannot execute; "+
					"the scenario loads and warns, but this phase does nothing",
					filepath.Base(path), ty)
			default:
				t.Errorf("%s declares event type %q, which the engine cannot execute; "+
					"scenario_load_file will reject the file. Fix the spelling or add "+
					"support for the type in load_events (sim_scenario.c)",
					filepath.Base(path), ty)
			}
		}
	}
}

// TestUnknownEventTypeFailsScenarioLoad is the direct test of the #166 fix:
// a scenario carrying an event type the engine cannot execute must fail to
// load rather than load and quietly drop the event. The scenario used here is
// a valid two-node line whose only defect is the event spelling, so a
// successful load would mean the engine had gone back to skipping in silence.
func TestUnknownEventTypeFailsScenarioLoad(t *testing.T) {
	const bad = `{
  "name": "Unknown Event Type",
  "duration_ms": 1000,
  "seed": 42,
  "radio": { "range": 300, "loss_pct": 0, "propagation_speed_ms_per_unit": 0.1 },
  "nodes": [ { "id": "A", "x": 0, "y": 0 }, { "id": "B", "x": 100, "y": 0 } ],
  "events": [ { "type": "node_move", "at_ms": 500, "node": "B", "x": 200, "y": 0 } ]
}`
	path := filepath.Join(t.TempDir(), "unknown-event-type.json")
	if err := os.WriteFile(path, []byte(bad), 0o600); err != nil {
		t.Fatalf("write fixture: %v", err)
	}
	if _, err := runScenarioHeadless(path); err == nil {
		t.Fatal("scenario with an unrecognized event type loaded successfully; the engine is " +
			"skipping unknown events again, which is the root cause behind issues #144 and #166")
	}
}

// TestScenarioPathTraceMovementRuns pins the behaviour issue #166 restored:
// reliability-path-trace's two node_move events must reach the simulation.
//
// The assertions are derived from the scenario file and the radio config, not
// from a recorded run: the file moves C to (200, 300) at 45s and back to
// (200, 0) at 70s, and the engine must emit a node_moved for each. With
// range 120 on a 100-unit line, C at y=300 is 316 units from both B and D, so
// the move partitions A-B from D-E. That partition is the whole point of the
// movement phase, and before this fix none of it happened.
func TestScenarioPathTraceMovementRuns(t *testing.T) {
	result, err := runScenarioHeadless("../scenarios/reliability-path-trace.json")
	if err != nil {
		t.Fatalf("runScenarioHeadless: %v", err)
	}

	type move struct {
		node string
		x, y float64
		at   uint64
	}
	var moves []move
	for _, line := range result.Lines() {
		var evt map[string]interface{}
		if json.Unmarshal([]byte(line), &evt) != nil {
			continue
		}
		if evt["type"] != "node_moved" {
			continue
		}
		m := move{}
		m.node, _ = evt["node"].(string)
		if v, ok := evt["x"].(float64); ok {
			m.x = v
		}
		if v, ok := evt["y"].(float64); ok {
			m.y = v
		}
		if v, ok := evt["timestamp_us"].(float64); ok {
			m.at = uint64(v)
		}
		moves = append(moves, m)
	}

	want := []move{
		{node: "C", x: 200, y: 300, at: 45000000},
		{node: "C", x: 200, y: 0, at: 70000000},
	}
	if len(moves) != len(want) {
		t.Fatalf("node_moved events = %d, want %d (%v); the scenario's movement phase is "+
			"inert again, which is exactly the regression issue #166 fixed", len(moves), len(want), moves)
	}
	for i, w := range want {
		if moves[i] != w {
			t.Errorf("node_moved[%d] = %+v, want %+v", i, moves[i], w)
		}
	}
}
