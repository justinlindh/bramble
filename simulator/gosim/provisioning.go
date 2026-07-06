package main

import (
	"encoding/json"
	"os"
)

// unprovisionedConfigJSON reads the scenario's optional per-node
// "unprovisioned" flag (mandatory-provisioning Task 2). A node marked
//
//	{"id": "X", "x": 0, "y": 0, "unprovisioned": true}
//
// boots WITHOUT the network key and is INERT: it originates no
// network-key-authenticated frame (DATA, attestation) and drops every inbound
// frame, while the rest of the fleet meshes normally. Default false
// (provisioned), matching a firmware fleet where every node holds the shared
// key. Read Go-side like the other scenario extensions (loadFloodTransportConfig
// in flood.go), so no C-side sim_scenario change is needed.
type unprovisionedConfigJSON struct {
	Nodes []struct {
		ID            string `json:"id"`
		Unprovisioned bool   `json:"unprovisioned"`
	} `json:"nodes"`
}

// loadUnprovisionedNodeIDs returns the set of node IDs marked unprovisioned in
// the scenario file. Any read/parse failure (or a scenario with no such field)
// returns an empty set (all nodes provisioned), the same fail-open-to-today's-
// default convention as loadFloodTransportConfig / loadIntermediateRREPConfig.
func loadUnprovisionedNodeIDs(path string) map[string]bool {
	out := map[string]bool{}
	data, err := os.ReadFile(path)
	if err != nil {
		return out
	}
	var cfg unprovisionedConfigJSON
	if err := json.Unmarshal(data, &cfg); err != nil {
		return out
	}
	for _, n := range cfg.Nodes {
		if n.Unprovisioned {
			out[n.ID] = true
		}
	}
	return out
}

// unendorsedConfigJSON reads the scenario's optional per-node "unendorsed" flag
// (trust-anchor campaign P2). A node marked
//
//	{"id": "X", "x": 0, "y": 0, "unendorsed": true}
//
// boots WITHOUT a fleet-anchor endorsement cert: it holds and uses the network
// key (so its attestations still MAC-verify and relay), but carries not_after=0
// on the wire, so every anchored receiver refuses to PIN it while the rest of
// the fleet meshes normally. Default false (endorsed), matching a fleet whose
// nodes were all enrolled by the anchor holder.
type unendorsedConfigJSON struct {
	Nodes []struct {
		ID         string `json:"id"`
		Unendorsed bool   `json:"unendorsed"`
	} `json:"nodes"`
}

// loadUnendorsedNodeIDs returns the set of node IDs marked unendorsed in the
// scenario file. Any read/parse failure (or a scenario with no such field)
// returns an empty set (all nodes endorsed), the same fail-open-to-today's-
// default convention as loadUnprovisionedNodeIDs.
func loadUnendorsedNodeIDs(path string) map[string]bool {
	out := map[string]bool{}
	data, err := os.ReadFile(path)
	if err != nil {
		return out
	}
	var cfg unendorsedConfigJSON
	if err := json.Unmarshal(data, &cfg); err != nil {
		return out
	}
	for _, n := range cfg.Nodes {
		if n.Unendorsed {
			out[n.ID] = true
		}
	}
	return out
}
