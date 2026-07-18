package main

import (
	"encoding/json"
	"os"
)

// nodeFlagConfigJSON reads a scenario file just far enough to recover each
// node's id plus its optional per-node boolean flags. Flag values are kept as
// raw JSON so a single loader can pull out whichever flag a caller names,
// instead of one typed struct + loader per flag.
type nodeFlagConfigJSON struct {
	Nodes []map[string]json.RawMessage `json:"nodes"`
}

// loadNodeFlagIDs returns the set of node IDs whose named boolean flag is true
// in the scenario file. Any read/parse failure (or a scenario with no such
// field) returns an empty set, the fail-open-to-today's-default convention
// shared with loadFloodTransportConfig / loadIntermediateRREPConfig.
//
// The recognised flags select which nodes boot in a degraded trust state; every
// flag defaults false, matching a fleet where each node is fully provisioned:
//
//   - "unprovisioned" (mandatory-provisioning Task 2): boots WITHOUT the network
//     key and is INERT. It originates no network-key-authenticated frame (DATA,
//     attestation) and drops every inbound frame, while the rest of the fleet
//     meshes normally.
//   - "unendorsed" (trust-anchor campaign P2): boots WITHOUT a fleet-anchor
//     endorsement cert. It holds and uses the network key (so its attestations
//     still MAC-verify and relay) but carries not_after=0 on the wire, so every
//     anchored receiver refuses to PIN it.
//   - "unanchored" (trust-anchor campaign P2 red-team): boots WITHOUT a fleet
//     anchor, pinning on self-sig alone (TOFU) and ignoring cert fields, exactly
//     like a node deployed before the operator provisioned an anchor. A later
//     "provision_anchor" event anchors it and DROPS those stale pins.
//
// Read Go-side like the other scenario extensions (loadFloodTransportConfig in
// flood.go), so no C-side sim_scenario change is needed.
func loadNodeFlagIDs(path, flag string) map[string]bool {
	out := map[string]bool{}
	data, err := os.ReadFile(path)
	if err != nil {
		return out
	}
	var cfg nodeFlagConfigJSON
	if err := json.Unmarshal(data, &cfg); err != nil {
		return out
	}
	for _, n := range cfg.Nodes {
		var set bool
		if raw, ok := n[flag]; ok {
			_ = json.Unmarshal(raw, &set)
		}
		if !set {
			continue
		}
		var id string
		if raw, ok := n["id"]; ok {
			_ = json.Unmarshal(raw, &id)
		}
		if id != "" {
			out[id] = true
		}
	}
	return out
}
