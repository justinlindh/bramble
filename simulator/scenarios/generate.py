#!/usr/bin/env python3
"""
Parameterized scenario generator for gosim.

Supersedes generate-adaptive-scenarios.py (deleted; --legacy reproduces its
four outputs exactly). Emits scenario JSON compatible with
simulator/engine/sim_scenario.c's scenario_load_file, including the
Task 3 "beacon" block and Task 5 "radio.duty_cycle_pct" field.

Design rule: every schema field that has a real firmware/sim default
(sf, bw_hz, range, beacon.*, radio.duty_cycle_pct, seed) is emitted ONLY if
its CLI flag was explicitly given. Omitted fields let the sim apply its own
default (radio_config_init / sim_beacon_policy_init / scenario_load_file's
seed=42 fallback), exactly as if the scenario file had never mentioned
them. This is what makes --legacy reproduce the old files byte-for-byte
(modulo "range": the old generator hardcoded "range": 150; this one omits it
so range derives from the sf/bw link budget instead of being a fixed disk
decoupled from them -- at the default PHY the derivation lands at ~150 units
by construction, since the sensitivity model's calibration anchor is defined
as "the frequency plan's default PHY derives the 150-unit baseline"
(radio_noise_margin_db in sim_radio.c), so --legacy's runtime behavior is
unchanged even though the emitted JSON no longer has the literal field, and
stays unchanged if a plan's default SF moves. Pass --range to force the old
fixed-disk field back on.

Legacy mapping (--legacy {10,50,100,200}), matching the deleted
generate-adaptive-scenarios.py's behavior:
    --topology grid --spacing 120 --duration-s 600
    --traffic-msgs-per-min 2 --nodes {10,50,100,200}
    (sf/bw/beacon/duty/seed/range all omitted)
    name: airtime-adaptive-{N}, out: airtime-adaptive-{N}.json
"""

import argparse
import json
import math
import random
import sys

LEGACY_SPACING = 120
LEGACY_DURATION_S = 600
LEGACY_TRAFFIC_MSGS_PER_MIN = 2
DEFAULT_RANDOM_SEED = 42  # only for --topology random node placement; not written to output


def grid_nodes(count, spacing):
    cols = math.ceil(math.sqrt(count))
    nodes = []
    for i in range(count):
        row = i // cols
        col = i % cols
        nodes.append({
            "id": f"N{i + 1:03d}",
            "x": col * spacing,
            "y": row * spacing,
        })
    return nodes


def line_nodes(count, spacing):
    return [
        {"id": f"N{i + 1:03d}", "x": i * spacing, "y": 0}
        for i in range(count)
    ]


def random_nodes(count, spacing, seed):
    # Scatter uniformly over a square sized so average node density
    # matches the grid layout at the same spacing and count.
    rng = random.Random(seed if seed is not None else DEFAULT_RANDOM_SEED)
    side = spacing * math.ceil(math.sqrt(count))
    return [
        {
            "id": f"N{i + 1:03d}",
            "x": round(rng.uniform(0, side), 2),
            "y": round(rng.uniform(0, side), 2),
        }
        for i in range(count)
    ]


def generate_nodes(topology, count, spacing, seed):
    if topology == "grid":
        return grid_nodes(count, spacing)
    if topology == "line":
        return line_nodes(count, spacing)
    if topology == "random":
        return random_nodes(count, spacing, seed)
    raise ValueError(f"unknown topology: {topology}")


def generate_messages(nodes, duration_ms, msgs_per_min):
    """Deterministic round-robin src/dest pairing, one message every
    60000/msgs_per_min ms starting at 10s, stopping 10s before the end.
    Reproduces generate-adaptive-scenarios.py's generate_messages exactly
    at msgs_per_min=2 (the value every legacy scenario used)."""
    events = []
    if msgs_per_min <= 0:
        return events
    msg_interval_ms = 60000 / msgs_per_min
    node_count = len(nodes)
    if node_count < 2:
        return events

    time_ms = 10000
    msg_id = 0
    while time_ms < duration_ms - 10000:
        src_idx = msg_id % node_count
        dest_idx = (msg_id + node_count // 2 + 1) % node_count
        if src_idx != dest_idx:
            events.append({
                "at_ms": int(time_ms),
                "type": "send_message",
                "src": nodes[src_idx]["id"],
                "dest": nodes[dest_idx]["id"],
            })
        time_ms += msg_interval_ms
        msg_id += 1
    return events


def build_scenario(args):
    nodes = generate_nodes(args.topology, args.nodes, args.spacing, args.seed)
    duration_ms = args.duration_s * 1000
    events = generate_messages(nodes, duration_ms, args.traffic_msgs_per_min)

    radio = {
        "loss_pct": 0,
        "propagation_speed_ms_per_unit": 0.1,
    }
    # "range" is omitted unless explicitly given: the sim derives it from the
    # link budget implied by sf/bw_hz (simulator/engine/sim_radio.c
    # radio_derive_range), so a radio-knob scenario gets SF/BW physically
    # coupled to reception range instead of a fixed disk. Pass --range to
    # keep the old fixed-disk behavior for topology tests that want range
    # decoupled from SF/BW.
    if args.range is not None:
        radio["range"] = args.range
    if args.sf is not None:
        radio["sf"] = args.sf
    if args.bw is not None:
        radio["bw_hz"] = args.bw
    if args.duty_cycle_pct is not None:
        radio["duty_cycle_pct"] = args.duty_cycle_pct

    scenario = {
        "name": args.name,
        "mode": "deterministic",
        "duration_ms": duration_ms,
        "nodes": nodes,
        "radio": radio,
        "events": events,
    }
    if args.seed is not None:
        scenario["seed"] = args.seed

    # Phase 2 Task 0 (flood-comparison baseline): "routing" selects gosim's
    # routing mode ("reactive", the default, or "flood", the Meshtastic-
    # style managed-flooding mode, simulator/gosim/flood.go); omitted unless
    # explicitly given, same emit-only-if-explicit rule as everything else
    # in this generator, so existing scenarios' interpretation never changes.
    if args.routing is not None:
        scenario["routing"] = args.routing
    if args.flood_hop_limit is not None:
        scenario["flood_hop_limit"] = args.flood_hop_limit

    # Phase 2 "save reactive routing" Part B: "intermediate_rrep" A/B
    # switch (gosim/bridge.h's bridge_set_intermediate_rrep_enabled);
    # omitted unless explicitly given, same emit-only-if-explicit rule.
    if args.intermediate_rrep is not None:
        scenario["intermediate_rrep"] = bool(args.intermediate_rrep)

    beacon = {}
    if args.beacon_adaptive is not None:
        beacon["adaptive"] = bool(args.beacon_adaptive)
    if args.beacon_interval_ms is not None:
        beacon["interval_ms"] = args.beacon_interval_ms
    if beacon:
        scenario["beacon"] = beacon

    return scenario


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--nodes", type=int, default=10, help="node count (default: 10)")
    p.add_argument("--spacing", type=float, default=120,
                  help="grid/line spacing, or random-topology density reference, in grid units "
                       "(default: 120)")
    p.add_argument("--topology", choices=["grid", "line", "random"], default="grid",
                  help="node layout (default: grid)")
    p.add_argument("--seed", type=int, default=None,
                  help="RNG seed for random-topology placement and the sim's own stochastic RNG; "
                       "omitted from the scenario file (sim defaults to 42) unless given")
    p.add_argument("--sf", type=int, choices=range(7, 13), default=None,
                  help="LoRa spreading factor; omitted (sim uses the frequency plan's "
                       "default, SF9) unless given")
    p.add_argument("--bw", type=int, choices=[125000, 250000], default=None,
                  help="LoRa bandwidth Hz; omitted (sim default 125000) unless given")
    p.add_argument("--range", type=float, default=None,
                  help="fixed reception-range override, grid units; omitted (sim derives range "
                       "from the sf/bw link budget, see sim_radio.c radio_derive_range) unless "
                       "given. Set this to decouple range from sf/bw, e.g. for topology tests")
    p.add_argument("--beacon-interval-ms", type=int, default=None,
                  help="fixed/base beacon interval ms; omitted (sim default 60000) unless given")
    p.add_argument("--beacon-adaptive", type=int, choices=[0, 1], default=None,
                  help="1 = opt into the adaptive beacon policy; omitted (sim default: fixed, "
                       "firmware's shipped BEACON_MODE_FIXED) unless given")
    p.add_argument("--duty-cycle-pct", type=float, default=None,
                  help="optional regulatory duty-cycle cap 0-100; omitted (unlimited, today's "
                       "behavior) unless given")
    p.add_argument("--routing", choices=["reactive", "flood"], default=None,
                  help="gosim routing mode; omitted (sim default: reactive, Bramble's real "
                       "firmware AODV path) unless given. 'flood' selects the Phase 2 Task 0 "
                       "Meshtastic-style managed-flooding sim-layer mode (simulator/gosim/flood.go)")
    p.add_argument("--flood-hop-limit", type=int, default=None,
                  help="flood mode's hop_limit; omitted (sim default: 3, Meshtastic's shipped "
                       "default) unless given. Ignored when --routing is not 'flood'")
    p.add_argument("--intermediate-rrep", type=int, choices=[0, 1], default=None,
                  help="Phase 2 Part B A/B switch: 1 = intermediate-node RREP on, 0 = off; "
                       "omitted (sim default: on, matching firmware's always-on shipped "
                       "behavior) unless given. Ignored in flood mode (reactive-only feature)")
    p.add_argument("--traffic-msgs-per-min", type=float, default=LEGACY_TRAFFIC_MSGS_PER_MIN,
                  help=f"message generation rate (default: {LEGACY_TRAFFIC_MSGS_PER_MIN})")
    p.add_argument("--duration-s", type=int, default=LEGACY_DURATION_S,
                  help=f"scenario duration in seconds (default: {LEGACY_DURATION_S})")
    p.add_argument("--name", default=None, help="scenario name (default: derived from topology/nodes)")
    p.add_argument("--out", default=None, help="output file path (required unless --legacy sets one)")
    p.add_argument("--legacy", type=int, choices=[10, 50, 100, 200], default=None,
                  help="reproduce the deleted generate-adaptive-scenarios.py output for N nodes "
                       "exactly (grid/spacing=120/duration=600s/traffic=2msg/min, no sf/bw/beacon/"
                       "duty/seed overrides); sets --nodes, --out and --name unless overridden")

    args = p.parse_args(argv)

    if args.legacy is not None:
        args.nodes = args.legacy
        args.topology = "grid"
        args.spacing = LEGACY_SPACING
        args.duration_s = LEGACY_DURATION_S
        args.traffic_msgs_per_min = LEGACY_TRAFFIC_MSGS_PER_MIN
        if args.name is None:
            args.name = f"airtime-adaptive-{args.legacy}"
        if args.out is None:
            args.out = f"airtime-adaptive-{args.legacy}.json"

    if args.name is None:
        args.name = f"{args.topology}-{args.nodes}"
    if args.out is None:
        p.error("--out is required (or use --legacy, which derives it)")

    return args


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
    scenario = build_scenario(args)
    with open(args.out, "w") as f:
        json.dump(scenario, f, indent=2)
    print(f"Generated {args.out}: {args.nodes} nodes ({args.topology}), "
          f"{len(scenario['events'])} messages, {args.duration_s}s duration")


if __name__ == "__main__":
    main()
