package main

/*
#include <stdlib.h>
#include "bridge.h"
*/
import "C"

// Mesh digital twin: taking exports off a finished simulation run.
//
// A simulated node keeps the same neighbour_table_t and routing_table_t a real
// one does, so it can answer bramble.exportTopology through the firmware's own
// document builder (main/topology_export.c, compiled into the sim via all.c).
// That is what the twin's round-trip check rests on: the reconstruction is fed
// documents written by firmware code, not by a second implementation of the
// schema that could agree with itself and disagree with a device.

import "unsafe"

// twinObservedExport is one simulated node's export document, tagged with the
// scenario id of the node that produced it. The id matters because gosim
// derives a node's address from its scenario id (node_array_add), so the id is
// the stable handle for following one node across a re-import that re-derives
// addresses from scratch.
type twinObservedExport struct {
	ScenarioID string
	JSON       []byte
}

// captureTwinExports serializes every active node's observed mesh state at the
// simulation's current time, exactly as calling bramble.exportTopology on each
// device would.
func captureTwinExports(s *Sim) []twinObservedExport {
	var out []twinObservedExport
	for i := 0; i < int(s.nodes.count); i++ {
		node := C.node_array_get(&s.nodes, C.int(i))
		if !bool(node.active) {
			continue
		}
		doc := C.bridge_export_topology(node, &s.radio, C.uint64_t(s.simTime))
		if doc == nil {
			continue
		}
		out = append(out, twinObservedExport{
			ScenarioID: C.GoString(&node.id[0]),
			JSON:       []byte(C.GoString(doc)),
		})
		C.free(unsafe.Pointer(doc))
	}
	return out
}

// TwinExports is captureTwinExports over a finished run, the Go-typed entry
// point _test.go files use (they avoid "C" directly; see radio_harness.go).
func (r *scenarioRunResult) TwinExports() []twinObservedExport {
	return captureTwinExports(r.sim)
}

// AudibleLinks is twinAudibleLinks over a finished run, the ground truth a
// reconstruction is compared against.
func (r *scenarioRunResult) AudibleLinks() map[[2]string]bool {
	return twinAudibleLinks(r.sim)
}

// twinAudibleLinks reports, for a finished run, every ordered node pair the
// radio model would actually carry a frame across: the ground truth an
// imported reconstruction is measured against. Keys are scenario ids.
func twinAudibleLinks(s *Sim) map[[2]string]bool {
	out := map[[2]string]bool{}
	for i := 0; i < int(s.nodes.count); i++ {
		tx := C.node_array_get(&s.nodes, C.int(i))
		if !bool(tx.active) {
			continue
		}
		for j := 0; j < int(s.nodes.count); j++ {
			if i == j {
				continue
			}
			rx := C.node_array_get(&s.nodes, C.int(j))
			if !bool(rx.active) {
				continue
			}
			if bool(C.radio_audible(&s.radio, tx, rx)) {
				out[[2]string{C.GoString(&tx.id[0]), C.GoString(&rx.id[0])}] = true
			}
		}
	}
	return out
}
