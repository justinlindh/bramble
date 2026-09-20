package main

import (
	"reflect"
	"testing"
)

// TestLoadFirmwareNodesParsesSpecs pins the scenario "firmware_nodes" wire shape
// to firmwareNodeSpec's json tags: every field must round-trip through a direct
// unmarshal, including the nested positions and env map.
func TestLoadFirmwareNodesParsesSpecs(t *testing.T) {
	data := []byte(`{
		"firmware_nodes": [
			{"type": "firmware", "binary": "emulator/node/build/bramble-node",
			 "count": 3, "positions": [[0,0],[100,0],[50,80]],
			 "label": "pager", "env": {"NODE_ROLE": "gateway"}}
		]
	}`)
	got := loadFirmwareNodes(data)
	want := []firmwareNodeSpec{{
		Type:      "firmware",
		Binary:    "emulator/node/build/bramble-node",
		Count:     3,
		Positions: [][2]float32{{0, 0}, {100, 0}, {50, 80}},
		Label:     "pager",
		Env:       map[string]string{"NODE_ROLE": "gateway"},
	}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("loadFirmwareNodes mismatch:\n got %+v\nwant %+v", got, want)
	}
}

// TestLoadFirmwareNodesFiltersAndDefaults covers the two normalizations the
// loader applies: a group with no binary is dropped, and a missing or
// non-positive count defaults to 1.
func TestLoadFirmwareNodesFiltersAndDefaults(t *testing.T) {
	data := []byte(`{
		"firmware_nodes": [
			{"binary": "", "count": 5},
			{"binary": "node-a"},
			{"binary": "node-b", "count": 0},
			{"binary": "node-c", "count": -2}
		]
	}`)
	got := loadFirmwareNodes(data)
	want := []firmwareNodeSpec{
		{Binary: "node-a", Count: 1},
		{Binary: "node-b", Count: 1},
		{Binary: "node-c", Count: 1},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("loadFirmwareNodes filter/default mismatch:\n got %+v\nwant %+v", got, want)
	}
}

// TestLoadFirmwareNodesFailOpen returns nil for both malformed JSON and a
// scenario with no "firmware_nodes" key, keeping a pure harness scenario on the
// untouched virtual-time path.
func TestLoadFirmwareNodesFailOpen(t *testing.T) {
	if got := loadFirmwareNodes([]byte("not json")); got != nil {
		t.Fatalf("malformed JSON should return nil, got %+v", got)
	}
	if got := loadFirmwareNodes([]byte(`{"nodes": []}`)); got != nil {
		t.Fatalf("scenario without firmware_nodes should return nil, got %+v", got)
	}
}
