#!/usr/bin/env python3
"""
Traffic efficiency analysis utility
Processes captured traffic events and generates efficiency metrics
"""

import json
import sys
from collections import defaultdict, Counter
from dataclasses import dataclass
from typing import Dict, List

@dataclass
class TrafficMetrics:
    total_events: int = 0
    total_airtime_us: int = 0
    airtime_by_category: Dict[str, int] = None
    airtime_by_tier: Dict[str, int] = None
    airtime_by_pkt_type: Dict[int, int] = None
    tx_count: int = 0
    rx_count: int = 0
    packet_lengths: List[int] = None
    
    def __post_init__(self):
        if self.airtime_by_category is None:
            self.airtime_by_category = defaultdict(int)
        if self.airtime_by_tier is None:
            self.airtime_by_tier = defaultdict(int)
        if self.airtime_by_pkt_type is None:
            self.airtime_by_pkt_type = defaultdict(int)
        if self.packet_lengths is None:
            self.packet_lengths = []

def estimate_airtime_us(packet_len: int, is_tx: bool) -> int:
    """
    Estimate airtime based on packet length
    Using LoRa SF7 BW125 as baseline (~5.5 kbps effective)
    Preamble: ~20ms, Header: ~10ms, Payload: variable
    """
    # Conservative estimate for LoRa packet airtime
    preamble_us = 20000  # 20ms
    header_us = 10000    # 10ms
    # ~180 bytes/sec → ~5.5 us/byte
    payload_us = packet_len * 5500
    
    total = preamble_us + header_us + payload_us
    
    # TX includes processing overhead
    if is_tx:
        total += 5000  # 5ms processing
    
    return total

def analyze_events(events: List[dict]) -> TrafficMetrics:
    """Analyze captured traffic events"""
    metrics = TrafficMetrics()
    
    for event in events:
        metrics.total_events += 1
        
        # Estimate airtime
        packet_len = event.get("packet_len", 0)
        is_tx = event.get("is_tx", False)
        airtime = estimate_airtime_us(packet_len, is_tx)
        
        metrics.total_airtime_us += airtime
        
        # Categorize
        category = event.get("category", "unknown")
        tier = event.get("airtime_tier", "unknown")
        pkt_type = event.get("pkt_type", 0)
        
        metrics.airtime_by_category[category] += airtime
        metrics.airtime_by_tier[tier] += airtime
        metrics.airtime_by_pkt_type[pkt_type] += airtime
        
        if is_tx:
            metrics.tx_count += 1
        else:
            metrics.rx_count += 1
        
        metrics.packet_lengths.append(packet_len)
    
    return metrics

def format_report(metrics: TrafficMetrics, events: List[dict], input_file: str) -> str:
    """Generate markdown report"""
    
    if metrics.total_events == 0:
        return "# Error: No events to analyze\n"
    
    report = []
    report.append("# Bramble Traffic Efficiency Analysis Report")
    report.append(f"\n**Generated:** {__import__('datetime').datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    report.append(f"**Source:** `{input_file}`")
    report.append(f"\n## Summary\n")
    report.append(f"- **Total Events:** {metrics.total_events:,}")
    report.append(f"- **TX Events:** {metrics.tx_count:,} ({metrics.tx_count/metrics.total_events*100:.1f}%)")
    report.append(f"- **RX Events:** {metrics.rx_count:,} ({metrics.rx_count/metrics.total_events*100:.1f}%)")
    report.append(f"- **Total Airtime:** {metrics.total_airtime_us/1_000_000:.2f}s ({metrics.total_airtime_us:,}µs)")
    
    if metrics.packet_lengths:
        avg_len = sum(metrics.packet_lengths) / len(metrics.packet_lengths)
        report.append(f"- **Average Packet Length:** {avg_len:.1f} bytes")
        report.append(f"- **Max Packet Length:** {max(metrics.packet_lengths)} bytes")
    
    # Airtime by category
    report.append(f"\n## Airtime by Category\n")
    report.append("| Category | Airtime (s) | Airtime (%) | Events |")
    report.append("|----------|-------------|-------------|--------|")
    
    category_counts = Counter(e.get("category", "unknown") for e in events)
    for category, airtime_us in sorted(metrics.airtime_by_category.items(), 
                                       key=lambda x: x[1], reverse=True):
        airtime_s = airtime_us / 1_000_000
        pct = (airtime_us / metrics.total_airtime_us * 100) if metrics.total_airtime_us > 0 else 0
        count = category_counts[category]
        report.append(f"| {category} | {airtime_s:.3f}s | {pct:.1f}% | {count} |")
    
    # Broadcast bucket breakdown
    report.append(f"\n## Airtime by Tier (Bucket)\n")
    report.append("| Tier | Airtime (s) | Airtime (%) | Events |")
    report.append("|------|-------------|-------------|--------|")
    
    tier_counts = Counter(e.get("airtime_tier", "unknown") for e in events)
    for tier, airtime_us in sorted(metrics.airtime_by_tier.items(), 
                                   key=lambda x: x[1], reverse=True):
        airtime_s = airtime_us / 1_000_000
        pct = (airtime_us / metrics.total_airtime_us * 100) if metrics.total_airtime_us > 0 else 0
        count = tier_counts[tier]
        report.append(f"| {tier} | {airtime_s:.3f}s | {pct:.1f}% | {count} |")
    
    # Top packet types by airtime
    report.append(f"\n## Top Packet Types by Airtime\n")
    report.append("| Pkt Type | Airtime (s) | Airtime (%) | Events |")
    report.append("|----------|-------------|-------------|--------|")
    
    pkt_type_counts = Counter(e.get("pkt_type", 0) for e in events)
    top_types = sorted(metrics.airtime_by_pkt_type.items(), 
                      key=lambda x: x[1], reverse=True)[:10]
    
    for pkt_type, airtime_us in top_types:
        airtime_s = airtime_us / 1_000_000
        pct = (airtime_us / metrics.total_airtime_us * 100) if metrics.total_airtime_us > 0 else 0
        count = pkt_type_counts[pkt_type]
        report.append(f"| {pkt_type} | {airtime_s:.3f}s | {pct:.1f}% | {count} |")
    
    # Recommendations
    report.append(f"\n## Tuning Recommendations\n")
    
    # Analyze broadcast bucket usage
    broadcast_pct = (metrics.airtime_by_tier.get("broadcast", 0) / metrics.total_airtime_us * 100) if metrics.total_airtime_us > 0 else 0
    
    if broadcast_pct > 40:
        report.append(f"### 🔴 High Broadcast Bucket Usage ({broadcast_pct:.1f}%)\n")
        report.append("**Impact:** Broadcast budget draining faster than expected.\n")
        report.append("**Recommendations:**")
        
        beacon_airtime = metrics.airtime_by_category.get("beacon", 0)
        timesync_airtime = metrics.airtime_by_category.get("timesync", 0)
        
        if beacon_airtime > 0:
            beacon_pct = (beacon_airtime / metrics.total_airtime_us * 100)
            if beacon_pct > 20:
                report.append(f"- **Reduce beacon interval** (currently using {beacon_pct:.1f}% of total airtime)")
                report.append("  - Current: likely 30s → Suggested: 60s or adaptive")
                report.append(f"  - Expected savings: ~{beacon_airtime/2/1_000_000:.2f}s per capture period")
        
        if timesync_airtime > 0:
            timesync_pct = (timesync_airtime / metrics.total_airtime_us * 100)
            if timesync_pct > 10:
                report.append(f"- **Reduce timesync frequency** (currently using {timesync_pct:.1f}% of total airtime)")
                report.append("  - Current: likely 60s → Suggested: 120s or 300s")
                report.append(f"  - Expected savings: ~{timesync_airtime/2/1_000_000:.2f}s per capture period")
    
    elif broadcast_pct > 25:
        report.append(f"### 🟡 Moderate Broadcast Bucket Usage ({broadcast_pct:.1f}%)\n")
        report.append("**Status:** Within acceptable range but monitor for growth.\n")
    else:
        report.append(f"### 🟢 Healthy Broadcast Bucket Usage ({broadcast_pct:.1f}%)\n")
        report.append("**Status:** Well within limits.\n")
    
    # TX/RX ratio
    if metrics.tx_count > 0 and metrics.rx_count > 0:
        tx_rx_ratio = metrics.tx_count / metrics.rx_count
        report.append(f"\n### TX/RX Ratio: {tx_rx_ratio:.2f}\n")
        if tx_rx_ratio > 2:
            report.append("**Note:** High TX ratio may indicate excessive retransmissions or broadcasting.")
            report.append("- Review retry backoff settings")
            report.append("- Consider implementing adaptive beacon intervals based on neighbor density")
    
    # Packet size analysis
    if metrics.packet_lengths:
        avg_len = sum(metrics.packet_lengths) / len(metrics.packet_lengths)
        if avg_len > 150:
            report.append(f"\n### Large Average Packet Size ({avg_len:.1f} bytes)\n")
            report.append("**Recommendations:**")
            report.append("- Review payload compression opportunities")
            report.append("- Consider splitting large messages")
            report.append("- Audit metadata overhead")
    
    report.append(f"\n## Next Steps\n")
    report.append("1. Implement recommended tuning changes")
    report.append("2. Capture new baseline after changes")
    report.append("3. Compare before/after metrics")
    report.append("4. Monitor long-term trends (24h+ captures)")
    report.append("5. Consider implementing adaptive algorithms based on network density")
    
    return "\n".join(report)

def main():
    if len(sys.argv) < 2:
        print("Usage: traffic-analyze.py <input.jsonl> [output.md]")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None
    
    # Load events
    print(f"[+] Loading events from {input_file}")
    events = []
    try:
        with open(input_file, 'r') as f:
            for line in f:
                line = line.strip()
                if line:
                    events.append(json.loads(line))
    except Exception as e:
        print(f"[!] Error loading events: {e}")
        sys.exit(1)
    
    print(f"[+] Loaded {len(events)} events")
    
    # Analyze
    print("[+] Analyzing traffic patterns...")
    metrics = analyze_events(events)
    
    # Generate report
    print("[+] Generating report...")
    report = format_report(metrics, events, input_file)
    
    if output_file:
        with open(output_file, 'w') as f:
            f.write(report)
        print(f"[+] Report saved to {output_file}")
    else:
        print("\n" + "="*80)
        print(report)
        print("="*80)

if __name__ == "__main__":
    main()
