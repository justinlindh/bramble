# Bramble 5-Node Field Test Plan

## Hardware
- 5× Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)
- USB battery packs for mobile nodes
- Laptops with USB serial for monitoring (optional)

## Topology

```
    A ---- B ---- C
           |
           D ---- E
```

- **A**: Stationary base station (USB-powered, serial monitor)
- **B**: Central relay node, line-of-sight to A, C, D
- **C**: ~500m from B, no LOS to A
- **D**: ~300m from B, perpendicular direction
- **E**: ~500m from D, only reaches D

Ensure A↔C and A↔E have no direct link (use terrain/buildings as obstacles).

## Test Scenarios

### 1. Basic Connectivity (30 min)
- Power on all nodes sequentially (A→B→C→D→E)
- Verify beacons: each node discovers neighbors within 2 beacon intervals
- **Success:** Each node's neighbor table matches expected topology

### 2. Route Discovery (30 min)
- A sends message to C → expect RREQ→RREP via B
- A sends message to E → expect route A→B→D→E
- C sends message to E → expect route C→B→D→E
- **Success:** All routes established, route metrics reflect link quality

### 3. Multi-Hop Data Delivery (45 min)
- Send 20 messages A→C, measure delivery rate and latency
- Send 20 messages A→E (3-hop), measure delivery rate and latency
- Send 10 messages C→E simultaneously with A→E traffic
- **Success:** ≥95% delivery, latency <10s for 2-hop, <15s for 3-hop

### 4. Node Failure & Recovery (30 min)
- Power off B while A→C route is active
- Verify A receives RERR or detects broken route
- Power B back on, verify route re-establishes
- **Success:** Route error detected within 30s, recovery within 2 beacon intervals

### 5. Channel Messaging (30 min)
- All 5 nodes join channel "test-alpha" with shared PSK
- Node A broadcasts channel message → all nodes should receive via flood
- Node E broadcasts → verify multi-hop flood reaches A
- **Success:** 100% channel message delivery to all nodes

### 6. Mobility (30 min)
- Walk node C toward E while sending periodic messages from A
- Monitor route changes as C enters/exits range of different relays
- **Success:** Route adapts, no more than 2 consecutive lost messages during handoff

## Data Collection
- Serial logs at 115200 baud from all nodes (timestamped)
- Record per-node: beacons sent/received, RREQ/RREP counts, data messages sent/delivered
- Record RSSI/SNR from beacon logs for link quality analysis
- GPS coordinates of each node position

## Environment Notes
- Conduct in open park or campus with known obstacles
- Avoid heavy RF interference (away from WiFi APs, cell towers)
- Note weather conditions (humidity affects propagation)
- Test at LoRa SF10 BW125 CR4/5 (default Bramble config)

## Duration
~3 hours total including setup and teardown
