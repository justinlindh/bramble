# Bramble 20-Node Field Test Plan

## Hardware
- 20× Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)
- USB battery packs (10000mAh+, ~8hr runtime)
- 3× laptops with USB serial for base/monitor nodes
- Labeling tape (nodes numbered 01-20)

## Topology: Grid + Sparse Periphery

```
Core Grid (4×3):              Periphery:
01--02--03--04                17    18
|   |   |   |                 \   /
05--06--07--08                 15--16
|   |   |   |                /
09--10--11--12              19
    |                        \
    13--14                    20
```

- **Core (01-12):** 200m spacing in grid, each node reaches 2-4 neighbors
- **Bridge (13-14):** Extended arm from core, 300m spacing
- **Periphery (15-20):** Sparse tree topology, 400-500m spacing
- **Goal:** Max path length 6-7 hops (node 01 → node 20)

## Pre-Test Setup (1 hour)
1. Flash all 20 nodes with same firmware version
2. Assign unique addresses, all join channel "grid-test"
3. Pre-position nodes with GPS waypoints
4. Verify power levels and antenna connections
5. Designate nodes 01, 10, 20 as monitor nodes (serial logging)

## Test Scenarios

### Phase 1: Network Formation (45 min)
- Power on all nodes within 5 minutes
- Wait 10 minutes for beacon propagation
- **Collect:** Neighbor tables from monitor nodes
- **Success:** Every node has ≥1 neighbor, network is fully connected graph

### Phase 2: Routing Convergence (30 min)
- Node 01 initiates route discovery to nodes 12, 14, 20
- Node 20 initiates route discovery to nodes 01, 06
- Record time from RREQ origination to RREP receipt
- **Success:** All routes established within 30s, optimal hop counts (±1 of shortest path)

### Phase 3: Throughput Under Load (60 min)
- **Light load:** 01→20 sends 1 msg/min for 10 min (10 messages)
- **Medium load:** Add 05→16 and 12→19 sending 1 msg/min each (30 msgs total over 10 min)
- **Heavy load:** All peripheral nodes (15-20) send to node 01 at 1 msg/min simultaneously
- **Measure:** Delivery rate, latency distribution, airtime budget usage
- **Success:**
  - Light: ≥98% delivery, median latency <15s
  - Medium: ≥95% delivery, median latency <20s
  - Heavy: ≥85% delivery, no node airtime budget exhaustion

### Phase 4: Congestion Handling (30 min)
- Generate burst: 10 messages from node 01→20 in 30 seconds
- Monitor congestion packets from relay nodes
- Verify TX queue prioritization (routing control > data)
- **Success:** Congestion detected and signaled, routing packets not dropped

### Phase 5: Partition & Heal (45 min)
- Power off nodes 07 and 11 (splits grid into two halves)
- Verify 01→20 route breaks, RERR propagates
- Attempt 01→20 → should find alternate route via 08→12→13→14 or similar
- Power 07 and 11 back on
- Verify routes reconverge to shorter paths
- **Success:** Partition detected <60s, alternate route found <90s, reconvergence <120s

### Phase 6: Simultaneous Route Discovery (30 min)
- 5 nodes simultaneously start route discovery to 5 different destinations
- Monitor RREQ dedup efficiency (how many duplicate RREQs filtered)
- Monitor channel utilization via airtime counters
- **Success:** All 5 routes established, dedup filters >60% of redundant RREQs

### Phase 7: Channel Flood at Scale (30 min)
- All 20 nodes joined to channel "grid-test"
- Node 01 sends channel broadcast → measure flood propagation time to node 20
- Node 10 (center) sends broadcast → measure time to all edges
- Send 5 broadcasts, 2 minutes apart
- **Success:** 100% delivery, propagation time scales linearly with hop count (~2s/hop)

### Phase 8: Endurance (2 hours, can overlap with teardown)
- Leave network running with 01→20 pings every 5 minutes
- Monitor for: memory leaks (via uptime_min + free heap), routing table bloat, battery drain
- **Success:** No degradation over 2 hours, battery >50% remaining

## Data Collection

### Per Node (from serial logs)
| Metric | Collection |
|--------|-----------|
| Neighbor table | Dump every 5 min |
| Route table | Dump every 5 min |
| Beacons TX/RX | Counter |
| RREQ/RREP/RERR counts | Counter |
| Data msgs sent/delivered/failed | Counter |
| Airtime budget remaining | Sample every 1 min |
| RSSI/SNR per neighbor | From beacon logs |
| Free heap memory | Sample every 5 min |

### Aggregate Metrics
- End-to-end delivery rate per source-dest pair
- Latency distribution (p50, p90, p99) by hop count
- Route convergence time after topology change
- Airtime utilization per node (% of budget used)
- RREQ amplification factor (total RREQs / unique route discoveries)

### Equipment
- Timestamped serial capture: `picocom -b 115200 --logfile node_XX.log`
- GPS coordinates recorded per node (photo + Google Maps pin)
- Weather: temperature, humidity, wind
- Photos of each node placement and surroundings

## Site Requirements
- Open area ~1km × 600m (park, campus, fairground)
- Mix of LOS and NLOS paths (buildings, trees as natural obstacles)
- Minimal LoRa interference (check 915MHz with SDR before test)
- Vehicle access for node deployment/retrieval
- Power outlets for monitor nodes (or extra batteries)

## Team
- 4-5 people: 1 coordinator, 2-3 deployers, 1 monitor operator
- Walkie-talkies for team coordination (don't use the mesh for logistics!)

## Duration
- Setup: 1 hour
- Testing: ~4-5 hours
- Teardown: 30 minutes
- **Total: ~6 hours**

## Post-Test Analysis
1. Parse serial logs → generate per-node statistics CSV
2. Plot: delivery rate vs hop count, latency vs load, RSSI heatmap
3. Identify bottleneck nodes (highest airtime usage, most route errors)
4. Compare actual vs expected routing paths
5. Document any firmware bugs or unexpected behavior
