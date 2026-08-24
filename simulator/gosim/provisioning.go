package main

import (
	"encoding/json"
)

// nodeFlagConfigJSON reads the scenario bytes just far enough to recover each
// node's id plus its optional per-node trust flags. Each flag defaults false
// (absent == not set), so plain bools capture the "flag off" case; the three
// degraded trust states are a closed protocol concept, so they are named fields
// rather than an open flag map.
type nodeFlagConfigJSON struct {
	Nodes []struct {
		ID            string `json:"id"`
		Unprovisioned bool   `json:"unprovisioned"`
		Unendorsed    bool   `json:"unendorsed"`
		Unanchored    bool   `json:"unanchored"`
	} `json:"nodes"`
}

// nodeTrustFlags holds, for each degraded trust state, the set of node IDs the
// scenario marks with that state. See loadNodeTrustFlags for what each models.
type nodeTrustFlags struct {
	unprovisioned map[string]bool
	unendorsed    map[string]bool
	unanchored    map[string]bool
}

// loadNodeTrustFlags returns, for each degraded trust state, the set of node IDs
// whose corresponding flag is true in the scenario bytes. It parses the scenario
// once. Any parse failure (or a scenario with no such field) yields empty sets,
// the fail-open-to-today's-default convention shared with loadFloodTransportConfig
// / loadIntermediateRREPConfig.
//
// The flags select which nodes boot in a degraded trust state; every flag
// defaults false, matching a fleet where each node is fully provisioned:
//
//   - "unprovisioned": boots WITHOUT the network key and is INERT. It
//     originates no network-key-authenticated frame (DATA, attestation) and
//     drops every inbound frame, while the rest of the fleet meshes normally.
//   - "unendorsed": boots WITHOUT a fleet-anchor endorsement cert. It holds
//     and uses the network key (so its attestations still MAC-verify and
//     relay) but carries not_after=0 on the wire, so every anchored receiver
//     refuses to PIN it.
//   - "unanchored": boots WITHOUT a fleet anchor, pinning on self-sig alone
//     (TOFU) and ignoring cert fields, exactly like a node deployed before
//     the operator provisioned an anchor. A later "provision_anchor" event
//     anchors it and DROPS those stale pins.
//
// Read Go-side like the other scenario extensions (loadFloodTransportConfig in
// flood.go), so no C-side sim_scenario change is needed.
func loadNodeTrustFlags(data []byte) nodeTrustFlags {
	flags := nodeTrustFlags{
		unprovisioned: map[string]bool{},
		unendorsed:    map[string]bool{},
		unanchored:    map[string]bool{},
	}
	var cfg nodeFlagConfigJSON
	if err := json.Unmarshal(data, &cfg); err != nil {
		return flags
	}
	for _, n := range cfg.Nodes {
		if n.ID == "" {
			continue
		}
		if n.Unprovisioned {
			flags.unprovisioned[n.ID] = true
		}
		if n.Unendorsed {
			flags.unendorsed[n.ID] = true
		}
		if n.Unanchored {
			flags.unanchored[n.ID] = true
		}
	}
	return flags
}
