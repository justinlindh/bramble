package main

// Mesh digital twin: the device-side export document.
//
// bramble.exportTopology (main/rpc_methods.c, api/openapi.yaml) returns one
// node's view of the mesh: who it is, which neighbors it hears and at what
// link quality, its routing table, and the PHY that prices its time-on-air.
// This file is the parser for that document and nothing else; merging several
// of them into a link graph is twin_graph.go's job.
//
// The parser is deliberately strict. A twin built from a document it only
// half-understood would answer capacity and resilience questions about a mesh
// that does not exist, which is worse than refusing the file.

import (
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"strings"
)

// twinSchemaSupported is the bramble.exportTopology document version this
// importer understands, mirroring TOPOLOGY_EXPORT_SCHEMA in
// main/rpc_methods.c. A document from a newer firmware is refused rather than
// read with guessed semantics.
const twinSchemaSupported = 1

// twinExport is one node's export document.
type twinExport struct {
	Schema    int             `json:"twin_schema"`
	Node      twinExportNode  `json:"node"`
	Radio     twinExportRadio `json:"radio"`
	Neighbors []twinNeighbor  `json:"neighbors"`
	Routes    []twinRoute     `json:"routes"`
}

// twinExportNode identifies the exporting node.
type twinExportNode struct {
	Address         string `json:"address"`
	Name            string `json:"name,omitempty"`
	FirmwareVersion string `json:"firmware_version"`
	ProtocolVersion string `json:"protocol_version"`
	Hardware        string `json:"hardware"`
	UptimeS         uint64 `json:"uptime_s"`
}

// twinExportRadio is the exporting node's runtime PHY plus the frequency plan
// it was built for. sf, bw_hz and coding_rate are what price a frame's
// time-on-air; the plan's duty cycle bounds the airtime the deployment may
// spend.
type twinExportRadio struct {
	FrequencyMHz      float64 `json:"frequency_mhz"`
	SF                int     `json:"sf"`
	BWHz              int     `json:"bw_hz"`
	CodingRate        int     `json:"coding_rate"`
	TxPowerDBm        int     `json:"tx_power_dbm"`
	Region            string  `json:"region"`
	Regulatory        string  `json:"regulatory"`
	MaxDutyCyclePct   int     `json:"max_duty_cycle_pct"`
	DutyCycleEnforced bool    `json:"duty_cycle_enforced"`
}

// appliesDutyCap reports whether this plan imposes a real airtime ceiling the
// twin must honor. A cap counts only when it is enforced and lands strictly
// between 0 and 100: an advisory plan (not enforced), a 100% ceiling (no
// ceiling), and a malformed 0% all leave the twin at the sim's unlimited
// default. The scenario builder and the report share this predicate so they
// never disagree about whether a given plan is capped.
func (r twinExportRadio) appliesDutyCap() bool {
	return r.DutyCycleEnforced && r.MaxDutyCyclePct > 0 && r.MaxDutyCyclePct < 100
}

// twinNeighbor is one entry of the exporting node's neighbor table: a node it
// heard directly, and the link quality it heard that node at. The RSSI is
// measured AT THE EXPORTER, so the observation is directed: it describes the
// neighbor-to-exporter direction, not the reverse.
type twinNeighbor struct {
	Address          string `json:"address"`
	RSSI             int    `json:"rssi"`
	SNR              int    `json:"snr"`
	DeliveryRate     int    `json:"deliveryRate"`
	AirtimeRemaining int    `json:"airtimeRemaining"`
	LastSeenMs       uint64 `json:"last_seen_ms"`
	Name             string `json:"name,omitempty"`
}

// twinRoute is one routing-table entry. Routes name nodes the exporter can
// reach, which is not the same as nodes it can hear: a multi-hop route carries
// no link, so the importer reads routes for the addresses they mention rather
// than for reachability.
type twinRoute struct {
	Dest     string `json:"dest"`
	NextHop  string `json:"next_hop"`
	HopCount int    `json:"hop_count"`
	Metric   int    `json:"metric"`
	State    string `json:"state"`
	UseCount int    `json:"use_count"`
}

// rpcEnvelope is the JSON-RPC response an operator gets from a transport that
// does not unwrap results for them. Accepting both shapes means "save whatever
// the call returned" is a working instruction.
type rpcEnvelope struct {
	Result json.RawMessage `json:"result"`
	Error  *struct {
		Code    int    `json:"code"`
		Message string `json:"message"`
	} `json:"error"`
}

// parseTwinExport reads one export document. It accepts either the bare
// exportTopology result object or the whole JSON-RPC response that carried it.
// name is used only in error messages (normally the file path).
func parseTwinExport(data []byte, name string) (*twinExport, error) {
	var env rpcEnvelope
	if err := json.Unmarshal(data, &env); err != nil {
		return nil, fmt.Errorf("%s: not JSON: %w", name, err)
	}
	if env.Error != nil {
		return nil, fmt.Errorf("%s: holds an RPC error, not an export: %s", name, env.Error.Message)
	}
	body := data
	if len(env.Result) > 0 {
		body = env.Result
	}

	var exp twinExport
	if err := json.Unmarshal(body, &exp); err != nil {
		return nil, fmt.Errorf("%s: cannot decode export: %w", name, err)
	}
	if err := exp.validate(name); err != nil {
		return nil, err
	}
	return &exp, nil
}

// loadTwinExport reads and parses one export file.
func loadTwinExport(path string) (*twinExport, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return parseTwinExport(data, path)
}

// validate rejects anything the twin cannot honestly reconstruct.
func (e *twinExport) validate(name string) error {
	if e.Schema != twinSchemaSupported {
		return fmt.Errorf("%s: twin_schema %d, this importer reads %d", name, e.Schema,
			twinSchemaSupported)
	}
	addr, err := normalizeTwinAddr(e.Node.Address)
	if err != nil {
		return fmt.Errorf("%s: node address: %w", name, err)
	}
	e.Node.Address = addr

	if e.Radio.SF < 5 || e.Radio.SF > 12 {
		return fmt.Errorf("%s: radio.sf %d outside the LoRa range 5..12", name, e.Radio.SF)
	}
	if e.Radio.BWHz <= 0 {
		return fmt.Errorf("%s: radio.bw_hz %d must be positive", name, e.Radio.BWHz)
	}
	if e.Radio.CodingRate < 1 || e.Radio.CodingRate > 4 {
		return fmt.Errorf("%s: radio.coding_rate %d outside 1..4 (4/5 through 4/8)", name,
			e.Radio.CodingRate)
	}

	for i := range e.Neighbors {
		n := &e.Neighbors[i]
		na, err := normalizeTwinAddr(n.Address)
		if err != nil {
			return fmt.Errorf("%s: neighbor %d address: %w", name, i, err)
		}
		if na == addr {
			return fmt.Errorf("%s: lists itself (%s) as its own neighbor", name, na)
		}
		n.Address = na
		// The simulator's link table stores RSSI as a signed byte and reserves
		// zero for "no link", so an observation has to be a real, negative,
		// in-range reading before it can become an edge.
		if n.RSSI >= 0 || n.RSSI < -128 {
			return fmt.Errorf("%s: neighbor %s reports rssi %d, which is not a receivable "+
				"LoRa level (-128..-1 dBm)", name, na, n.RSSI)
		}
		if n.SNR < -128 || n.SNR > 127 {
			return fmt.Errorf("%s: neighbor %s reports snr %d outside -128..127 dB", name, na,
				n.SNR)
		}
	}

	for i := range e.Routes {
		r := &e.Routes[i]
		d, err := normalizeTwinAddr(r.Dest)
		if err != nil {
			return fmt.Errorf("%s: route %d dest: %w", name, i, err)
		}
		h, err := normalizeTwinAddr(r.NextHop)
		if err != nil {
			return fmt.Errorf("%s: route %d next_hop: %w", name, i, err)
		}
		r.Dest = d
		r.NextHop = h
	}
	return nil
}

// normalizeTwinAddr canonicalizes a Bramble address to 8 uppercase hex digits.
// A "0x" prefix is accepted because that is how gosim's own event stream
// renders addresses, and an operator pasting one in should not be punished.
func normalizeTwinAddr(s string) (string, error) {
	t := strings.TrimSpace(s)
	t = strings.TrimPrefix(strings.TrimPrefix(t, "0x"), "0X")
	if t == "" {
		return "", fmt.Errorf("empty")
	}
	v, err := strconv.ParseUint(t, 16, 32)
	if err != nil {
		return "", fmt.Errorf("%q is not a 32-bit hex address", s)
	}
	if v == 0 {
		return "", fmt.Errorf("%q is the null address", s)
	}
	return fmt.Sprintf("%08X", uint32(v)), nil
}
