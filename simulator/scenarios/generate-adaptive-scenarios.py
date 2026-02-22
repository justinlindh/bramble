#!/usr/bin/env python3
"""
Generate adaptive airtime test scenarios for 10/50/100/200 nodes.
Creates dense grid topologies to test adaptive beacon policy.
"""

import json
import math
import sys

def generate_grid_nodes(count):
    """Generate nodes in a grid layout with IDs like N001, N002, etc."""
    nodes = []
    # Compute grid dimensions (roughly square)
    cols = math.ceil(math.sqrt(count))
    rows = math.ceil(count / cols)
    spacing = 120  # Close enough for good connectivity
    
    for i in range(count):
        row = i // cols
        col = i % cols
        node_id = f"N{i+1:03d}"
        nodes.append({
            "id": node_id,
            "x": col * spacing,
            "y": row * spacing
        })
    
    return nodes

def generate_messages(nodes, duration_ms, msg_rate_per_min=2):
    """Generate message events spread throughout the simulation."""
    events = []
    msg_interval_ms = 60000 / msg_rate_per_min  # Convert per-minute rate to interval
    
    node_count = len(nodes)
    if node_count < 2:
        return events
    
    time_ms = 10000  # Start at 10s
    msg_id = 0
    
    while time_ms < duration_ms - 10000:
        # Pick random source and destination (deterministic using msg_id for reproducibility)
        src_idx = msg_id % node_count
        dest_idx = (msg_id + node_count // 2 + 1) % node_count
        
        if src_idx != dest_idx:
            events.append({
                "at_ms": int(time_ms),
                "type": "send_message",
                "src": nodes[src_idx]["id"],
                "dest": nodes[dest_idx]["id"]
            })
        
        time_ms += msg_interval_ms
        msg_id += 1
    
    return events

def generate_scenario(node_count, name_suffix=""):
    """Generate a complete scenario JSON."""
    nodes = generate_grid_nodes(node_count)
    
    # Duration: enough time for multiple beacon cycles
    # With 60s adaptive interval, run for ~10 minutes to see behavior
    duration_ms = 600000  # 10 minutes
    
    events = generate_messages(nodes, duration_ms, msg_rate_per_min=2)
    
    scenario = {
        "name": f"airtime-adaptive-{node_count}{name_suffix}",
        "mode": "deterministic",
        "duration_ms": duration_ms,
        "nodes": nodes,
        "radio": {
            "range": 150,
            "loss_pct": 0,
            "propagation_speed_ms_per_unit": 0.1
        },
        "events": events
    }
    
    return scenario

def main():
    counts = [10, 50, 100, 200]
    
    for count in counts:
        scenario = generate_scenario(count)
        filename = f"airtime-adaptive-{count}.json"
        
        with open(filename, 'w') as f:
            json.dump(scenario, f, indent=2)
        
        print(f"Generated {filename}: {count} nodes, {len(scenario['events'])} messages, {scenario['duration_ms']/1000:.0f}s duration")

if __name__ == "__main__":
    main()
