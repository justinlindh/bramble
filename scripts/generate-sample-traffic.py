#!/usr/bin/env python3
"""
Generate realistic sample traffic events for testing analysis pipeline
Simulates a 5-minute capture window with realistic packet patterns
"""

import json
import random
from datetime import datetime

# Packet type definitions based on Bramble protocol
PACKET_TYPES = {
    5: {"category": "beacon", "tier": "broadcast", "typical_len": 64},
    10: {"category": "timesync", "tier": "broadcast", "typical_len": 48},
    20: {"category": "chat", "tier": "normal", "typical_len": 128},
    21: {"category": "chat", "tier": "normal", "typical_len": 96},
    30: {"category": "routing", "tier": "broadcast", "typical_len": 72},
    31: {"category": "routing", "tier": "normal", "typical_len": 80},
    40: {"category": "ack", "tier": "critical", "typical_len": 32},
    50: {"category": "maintenance", "tier": "normal", "typical_len": 56},
    51: {"category": "maintenance", "tier": "broadcast", "typical_len": 64},
    99: {"category": "other", "tier": "normal", "typical_len": 100},
}

def generate_event(seq, base_timestamp_ms, is_tx):
    """Generate a single realistic traffic event"""
    
    # Weighted packet type selection (beacons and routing more common)
    pkt_type = random.choices(
        list(PACKET_TYPES.keys()),
        weights=[25, 15, 8, 5, 12, 6, 10, 4, 8, 2],  # beacon, timesync heavy
        k=1
    )[0]
    
    pkt_info = PACKET_TYPES[pkt_type]
    
    # Add realistic variance to packet length
    packet_len = pkt_info["typical_len"] + random.randint(-8, 16)
    packet_len = max(32, min(250, packet_len))  # Clamp to reasonable range
    
    # RSSI only for RX
    rssi = random.randint(-95, -60) if not is_tx else 0
    
    return {
        "seq": seq,
        "timestamp_ms": base_timestamp_ms,
        "pkt_type": pkt_type,
        "category": pkt_info["category"],
        "airtime_tier": pkt_info["tier"],
        "packet_len": packet_len,
        "rssi": rssi,
        "is_tx": is_tx
    }

def generate_capture(duration_seconds=300, output_file="sample-traffic.jsonl"):
    """Generate a complete capture session"""
    
    print(f"[+] Generating {duration_seconds}s simulated capture")
    
    events = []
    seq = 1
    base_time = 1000000  # arbitrary start time in ms
    
    # Traffic patterns
    # Beacons: every 30s
    # Timesync: every 60s
    # Chat: sporadic bursts
    # Routing: periodic updates + route discoveries
    # ACKs: follow TX packets
    # Maintenance: occasional
    
    current_time = base_time
    end_time = base_time + (duration_seconds * 1000)
    
    last_beacon_tx = 0
    last_timesync_tx = 0
    last_routing_tx = 0
    
    while current_time < end_time:
        # Beacon TX every 30s
        if current_time - last_beacon_tx >= 30000:
            events.append(generate_event(seq, current_time, True))
            seq += 1
            last_beacon_tx = current_time
            # Simulate RX beacons from neighbors (1-3 other nodes)
            for _ in range(random.randint(1, 3)):
                current_time += random.randint(100, 500)
                events.append({
                    "seq": seq,
                    "timestamp_ms": current_time,
                    "pkt_type": 5,
                    "category": "beacon",
                    "airtime_tier": "broadcast",
                    "packet_len": 64 + random.randint(-4, 8),
                    "rssi": random.randint(-85, -65),
                    "is_tx": False
                })
                seq += 1
        
        # Timesync TX every 60s
        if current_time - last_timesync_tx >= 60000:
            events.append(generate_event(seq, current_time, True))
            seq += 1
            last_timesync_tx = current_time
            # RX timesync from coordinator
            current_time += random.randint(50, 200)
            events.append({
                "seq": seq,
                "timestamp_ms": current_time,
                "pkt_type": 10,
                "category": "timesync",
                "airtime_tier": "broadcast",
                "packet_len": 48,
                "rssi": random.randint(-75, -55),
                "is_tx": False
            })
            seq += 1
        
        # Routing updates every 45s + occasional discoveries
        if current_time - last_routing_tx >= 45000 or random.random() < 0.01:
            # Routing broadcast
            events.append({
                "seq": seq,
                "timestamp_ms": current_time,
                "pkt_type": 30,
                "category": "routing",
                "airtime_tier": "broadcast",
                "packet_len": 72 + random.randint(-8, 12),
                "rssi": 0,
                "is_tx": True
            })
            seq += 1
            last_routing_tx = current_time
        
        # Sporadic chat messages (every 20-90s)
        if random.random() < 0.02:
            # TX chat
            events.append({
                "seq": seq,
                "timestamp_ms": current_time,
                "pkt_type": 20,
                "category": "chat",
                "airtime_tier": "normal",
                "packet_len": random.randint(80, 180),
                "rssi": 0,
                "is_tx": True
            })
            seq += 1
            current_time += random.randint(200, 500)
            
            # ACK back
            events.append({
                "seq": seq,
                "timestamp_ms": current_time,
                "pkt_type": 40,
                "category": "ack",
                "airtime_tier": "critical",
                "packet_len": 32,
                "rssi": random.randint(-80, -60),
                "is_tx": False
            })
            seq += 1
        
        # Occasional maintenance
        if random.random() < 0.005:
            events.append(generate_event(seq, current_time, random.choice([True, False])))
            seq += 1
        
        # Advance time by random interval (100-1000ms)
        current_time += random.randint(100, 1000)
    
    # Sort by timestamp and reassign sequential seq
    events.sort(key=lambda e: e["timestamp_ms"])
    for i, event in enumerate(events, 1):
        event["seq"] = i
    
    # Write to JSONL
    with open(output_file, 'w') as f:
        for event in events:
            f.write(json.dumps(event) + "\n")
    
    print(f"[+] Generated {len(events)} events")
    print(f"[+] Time span: {(events[-1]['timestamp_ms'] - events[0]['timestamp_ms'])/1000:.1f}s")
    print(f"[+] Saved to: {output_file}")
    
    # Quick stats
    tx_count = sum(1 for e in events if e["is_tx"])
    rx_count = len(events) - tx_count
    print(f"[+] TX: {tx_count}, RX: {rx_count}")
    
    from collections import Counter
    categories = Counter(e["category"] for e in events)
    print(f"[+] Categories: {dict(categories)}")

if __name__ == "__main__":
    import sys
    output = sys.argv[1] if len(sys.argv) > 1 else "sample-traffic.jsonl"
    generate_capture(300, output)
