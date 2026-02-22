# Traffic Analysis Tooling Usage Guide

## Overview

This directory contains traffic efficiency analysis reports and captured data. The tooling enables end-to-end traffic observability through capture, analysis, and reporting.

## Scripts

### 1. `scripts/traffic-capture.sh`

Captures live traffic events from a Bramble device via WebSocket + RPC.

**Usage:**
```bash
# Capture from device for 5 minutes (default)
bash scripts/traffic-capture.sh 192.168.1.64

# Custom duration (in seconds) and output file
bash scripts/traffic-capture.sh 192.168.1.64 600 my-capture.jsonl

# Full syntax
bash scripts/traffic-capture.sh <device-ip> [duration-seconds] [output-file]
```

**Requirements:**
- Device must have traffic debug RPC methods implemented (Tasks 1-4)
- Python 3 with `websockets` module
- Network connectivity to device

**Output:** JSONL file with monotonic sequence numbers, one event per line

### 2. `scripts/traffic-analyze.py`

Processes captured JSONL traffic events and generates efficiency metrics.

**Usage:**
```bash
# Analyze and print to stdout
python3 scripts/traffic-analyze.py capture.jsonl

# Generate markdown report
python3 scripts/traffic-analyze.py capture.jsonl reports/my-report.md
```

**Metrics Produced:**
- Airtime by category (beacon, timesync, chat, routing, ack, maintenance)
- Airtime by tier (broadcast, normal, critical)
- Top packet types by airtime
- TX/RX ratio
- Packet size statistics
- Tuning recommendations based on thresholds

### 3. `scripts/generate-sample-traffic.py`

Generates realistic simulated traffic events for testing/development.

**Usage:**
```bash
# Generate 5-minute simulated capture
python3 scripts/generate-sample-traffic.py output.jsonl
```

**Use Cases:**
- Testing analysis pipeline before firmware deployment
- Demonstrating tooling capabilities
- Baseline comparison data

## Capture Data Format

Each event is a JSON object with:
```json
{
  "seq": 123,                    // monotonic sequence number
  "timestamp_ms": 1234567,       // millisecond timestamp
  "pkt_type": 5,                 // packet type ID
  "category": "beacon",          // traffic category
  "airtime_tier": "broadcast",   // airtime bucket
  "packet_len": 64,              // packet length in bytes
  "rssi": -75,                   // RSSI (0 for TX)
  "is_tx": false                 // true=TX, false=RX
}
```

## Current Reports

### `traffic-efficiency-2026-02-21.md`

**Source:** Simulated capture (300s, 74 events)

**Key Findings:**
- Broadcast bucket usage: 53.6% (HIGH - action needed)
- Beacons consuming 31.2% of total airtime
- Chat messages: 32.9% (largest category)
- TX/RX ratio: 1.00 (balanced)

**Primary Recommendation:** Reduce beacon interval from 30s to 60s (adaptive), expected 50% reduction in beacon airtime

## Workflow

1. **Capture**: Run `traffic-capture.sh` against target device for desired duration
2. **Analyze**: Run `traffic-analyze.py` on captured JSONL to generate report
3. **Review**: Examine recommendations in generated markdown report
4. **Tune**: Implement suggested configuration changes
5. **Validate**: Capture new baseline and compare metrics

## Limitations (Current Implementation)

- **Live Device Capture:** Requires firmware Tasks 1-4 to be deployed on device
  - Device at 192.168.1.64 does not yet have traffic debug methods
  - Current reports use simulated data to demonstrate tooling
- **Airtime Estimation:** Uses conservative LoRa SF7 BW125 formula (~5.5µs/byte)
  - Actual airtime may vary based on spreading factor and bandwidth
  - Future: extract actual airtime from firmware telemetry
- **Retry/ACK Analysis:** Limited to event counts (retry rate calculation needs extended schema)
- **Forwarding Overhead:** Not yet tracked in event schema

## Next Steps

1. Deploy firmware with traffic debug implementation to device
2. Run live capture session (30-60 minutes recommended)
3. Compare live vs simulated metrics
4. Iterate on tuning parameters
5. Add retry tracking to event schema (future enhancement)
