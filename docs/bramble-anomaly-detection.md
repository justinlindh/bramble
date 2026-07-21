# Bramble Anomaly Detection

The simulator detects four categories of mesh network anomalies. These represent real-world failure modes that can occur in deployed LoRa mesh networks. Not all anomalies have solutions yet; they exist to measure and anticipate problems.

## 1. Black Hole

**What:** A node receives DATA packets but fails to forward or deliver them. The packets silently disappear.

**When it happens:**

- Node loses its routing table (reboot, memory corruption) but neighbors still route through it
- Software bug causes a node to drop packets after receiving them
- Hardware failure where the transmitter stops working but receiver still functions

**Detection:** Track DATA packets received vs forwarded/delivered per node in a 10s sliding window. If a node receives ≥5 DATA packets and forwards 0, it's flagged.

**Simulator scenario:** `anomaly-black-hole.json`: Relay node C is killed and immediately re-joined (clearing its routing state). Neighbors still have stale routes through C, so DATA arrives but C has no routes to forward it. Detected at ~75s and ~85s.

**Notes:** Only DATA packets count. Beacons, RREQs, RREPs, and RERRs are excluded: a leaf node that receives many beacons but never forwards DATA is normal, not a black hole.

## 2. Mesh Partition

**What:** The mesh network splits into disconnected clusters. Nodes in different clusters cannot communicate.

**When it happens:**

- Bridge node (the only link between two groups) dies or moves out of range
- Physical obstruction blocks RF between clusters
- Interference zone isolates a region

**Detection:** After any `node_leave` event, BFS traversal from an arbitrary active node checks reachability of all other active nodes using Euclidean distance ≤ radio range. If any node is unreachable, the partition is reported with a list of isolated nodes.

**Simulator scenario:** `anomaly-partition.json`: Linear topology A-B-C-D-E with range 150 and spacing 100. Killing bridge node C splits {A,B} from {D,E}. Detected immediately at kill time. After C rejoins, the mesh heals.

## 3. Excessive RREQ Retransmission

**What:** A node floods the network with repeated Route Request packets for the same destination, wasting airtime and battery.

**When it happens:**

- Destination node is dead or unreachable, but the source keeps retrying
- Network congestion causes RREP packets to be lost, so the source never learns the route
- Software bug in retry logic

**Detection:** Per-node, per-destination tracking of RREQ originations and retransmissions within a 10s sliding window. If ≥6 RREQs for the same destination are sent in 10s, flagged as excessive.

**Simulator scenario:** `anomaly-excessive-rreq.json`: Node D is killed at 1s (before any routes are established). A repeatedly sends messages to D every 6s. Each attempt triggers a fresh RREQ discovery (stale discovery cleared after 5s, retries up to MAX_RREQ_ATTEMPTS with 1.5s intervals). Results in 11+ detections on A.

**Notes:** The real firmware (discovery.c) limits attempts to MAX_RREQ_ATTEMPTS=3 per discovery. The simulator models this, plus stale discovery retry. In production, this anomaly indicates a need for backoff or destination-unreachable caching.

## 4. Route Loop

**What:** A DATA packet visits the same node twice, indicating circular routing. The packet bounces between nodes until hop_limit expires, wasting airtime.

**When it happens:**

- Packet corruption (bit flip) changes a next_hop address, creating an unintended route
- Race condition: simultaneous topology changes and route discoveries create transiently inconsistent routing tables
- Implementation bug in RREP route installation

**Detection:** Per-node tracking of DATA packet_ids seen in the last 5s. If the same packet_id arrives at a node twice, a route loop is flagged.

**Simulator scenario:** `anomaly-route-loop.json`: Rapid kill/rejoin cycles on bridge node C with simultaneous DATA traffic. **Result: 0 loops detected.** This validates that Bramble's RREP-based routing correctly prevents loops under normal conditions.

**Why it exists despite 0 detections:** The detector is a safety net. In ideal simulated conditions, Bramble's routing design prevents loops because:

- RREP routes are installed along the exact reverse path of the RREQ flood
- Each node installs next_hop as the node that forwarded the RREP to it (not a hop from the RREP payload)
- hop_limit provides a hard backstop if loops do occur

In real deployments, packet corruption, RF interference causing partial packet reception, and race conditions with mobile nodes could create conditions the simulator can't easily reproduce. The detector measures how often the hop_limit safety net activates.

---

## Summary Table

| Anomaly | Detection Window | Threshold | Scenario Result |
| --------- | ----------------- | ----------- | ---------------- |
| Black hole | 10s | ≥5 rx, 0 fwd | 2 detections ✅ |
| Mesh partition | On topology change | BFS disconnected | 1 detection ✅ |
| Excessive RREQ | 10s per dest | ≥6 retransmits | 11 detections ✅ |
| Route loop | 5s per packet_id | Same ID at same node | 0 detections ✅ (validates routing) |

## False Positive Prevention

- **Black hole:** Only counts DATA packets (not beacons, RREQs, RREPs, RERRs). Leaf nodes receiving many beacons but no DATA to forward are NOT flagged.
- **Partition:** Only runs after `node_leave` events, using physical radio range for adjacency (not routing table state).
- **Excessive RREQ:** Per-destination tracking prevents cross-contamination between different route discoveries.
- **Route loop:** 5s TTL on seen packet_ids prevents stale detections. Only DATA packets are tracked.

Verified: ideal-10-node (10 nodes, 49 messages) and ideal-massive (150 nodes, 64 messages) both produce **zero anomalies**.

## Message Retry and Drop Behavior

When a node tries to send a message but has no route to the destination (or the route is broken/discovering), the simulator:

1. **Initiates route discovery**: sends an RREQ flood (up to `MAX_RREQ_ATTEMPTS=3` per discovery round)
2. **Reschedules the message**: retries every 1.5 seconds
3. **Gives up after 20 retries** (~30 seconds): emits a `message_dropped` event with `reason: "retry_timeout"` and increments the dropped counter

If a route exists but the DATA packet is lost in transit (e.g., swallowed by a black hole node), the message is counted as "sent" but never "delivered." The simulation tracks this as `undelivered` in final metrics:

- **`messages_sent`**: total messages that produced a DATA packet
- **`delivered`**: messages confirmed received at the destination
- **`undelivered`**: `messages_sent - delivered` (route existed but delivery failed)
- **`dropped`**: messages that could never find a route and timed out

Messages pending retry when the simulation ends are also counted as dropped.

**Note:** gosim now runs the firmware's real reliability machinery (3-tier: fire-and-forget, acknowledged, reliable) and measures ACK retransmission and receipt return paths under loss; see the `reliability-ack-retry` and `reliability-path-trace` scenarios, both gated in `simulator/gosim/scenario_gate_test.go`, and [results/simulation-2026-07-honest-baseline.md](results/simulation-2026-07-honest-baseline.md). The detection timings quoted in this document are from the February simulation runs that introduced the detectors and have not been re-measured since.
