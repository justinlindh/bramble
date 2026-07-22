package main

import (
	"encoding/json"
)

// firmwareNodesConfigJSON reads the scenario's optional top-level
// "firmware_nodes" array (Task 7). Each entry declares a full-firmware external
// node group:
//
//	"firmware_nodes": [
//	  {"type": "firmware", "binary": "emulator/node/build/bramble-node",
//	   "count": 3, "positions": [[0,0],[100,0],[50,80]], "label": "pager"}
//	]
//
// It is parsed Go-side, exactly like the other scenario extensions
// (loadNodeFlagIDs, loadRoutingConfig), so the C scenario loader
// (sim_scenario.c) needs no change and every existing scenario, which has no
// "firmware_nodes" key, is entirely unaffected.
type firmwareNodesConfigJSON struct {
	FirmwareNodes []struct {
		Type      string            `json:"type"`
		Binary    string            `json:"binary"`
		Count     int               `json:"count"`
		Positions [][2]float32      `json:"positions"`
		Label     string            `json:"label"`
		Env       map[string]string `json:"env"`
	} `json:"firmware_nodes"`
}

// loadFirmwareNodes returns the firmware-node groups declared in the scenario
// bytes. Any parse failure (or a scenario with no "firmware_nodes" key) returns
// nil, the same fail-open-to-today's-default convention as the other Go-side
// scenario loaders, so a pure harness scenario stays on the untouched
// virtual-time path.
func loadFirmwareNodes(data []byte) []firmwareNodeSpec {
	var cfg firmwareNodesConfigJSON
	if err := json.Unmarshal(data, &cfg); err != nil {
		return nil
	}
	var out []firmwareNodeSpec
	for _, n := range cfg.FirmwareNodes {
		if n.Binary == "" {
			continue
		}
		count := n.Count
		if count <= 0 {
			count = 1
		}
		out = append(out, firmwareNodeSpec{
			Type:      n.Type,
			Binary:    n.Binary,
			Count:     count,
			Positions: n.Positions,
			Label:     n.Label,
			Env:       n.Env,
		})
	}
	return out
}
