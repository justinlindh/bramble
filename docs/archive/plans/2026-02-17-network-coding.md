# Opportunistic Network Coding

> ✅ **SIMULATOR IMPLEMENTATION COMPLETE** (2026-02-17) — Checkboxes below not updated but all tasks were implemented in the simulator branch.

**Date:** 2026-02-17
**Status:** Draft
**Component:** `components/coding/`
**Depends on:** forwarding queue, reliability layer, airtime budget

## Summary

When a relay node has two packets queued in opposite directions through it (A→R→B and B→R→A), it can XOR the payloads and broadcast once instead of forwarding twice. Each receiver already has one component packet (the one it sent), so it XORs the coded packet to recover the other. One broadcast replaces two unicast forwards — a 50% airtime reduction for bidirectional flows through the same relay.

This is an adaptation of COPE (Katti et al., 2006) for LoRa mesh. The honest upfront take: COPE was designed for WiFi where queues are deep and packets are plentiful. LoRa's low data rate means coding opportunities may be rare in practice. This plan analyzes when it helps, when it doesn't, and whether the complexity is justified.

---

## 1. Core Concept

### The Classic Butterfly Example

```
    Alice                          Bob
   (node A)                     (node B)
      \                           /
       \   pkt_17: "hello bob"   /
        \                       /   pkt_23: "hey alice"
         \                     /
          \                   /
           +--- Relay (R) ---+
```

Without coding:
1. R forwards pkt_17 (A→B): ~480ms airtime
2. R forwards pkt_23 (B→A): ~480ms airtime
3. **Total: ~960ms airtime, 2 transmissions**

With coding:
1. R XORs pkt_17 ⊕ pkt_23, broadcasts coded packet: ~480ms airtime
2. Bob has pkt_23 (he sent it), computes: coded ⊕ pkt_23 = pkt_17 ✓
3. Alice has pkt_17 (she sent it), computes: coded ⊕ pkt_17 = pkt_23 ✓
4. **Total: ~480ms airtime, 1 transmission**

The key insight: each receiver already possesses one of the component packets. The XOR'd broadcast carries information for both simultaneously.

### Why This Works Over LoRa

LoRa's broadcast nature is actually an advantage here. In WiFi, COPE needs promiscuous mode to overhear packets. In LoRa, every transmission is inherently a broadcast — all nodes within range hear every packet. The relay's coded broadcast naturally reaches both Alice and Bob if they're within range (which they must be, since they're both one-hop neighbors of R).

---

## 2. Coding Opportunities

A relay node can code packets together when:

1. **Two packets are queued** going in opposite directions through this relay
2. **Each intended receiver already has** the other packet in the pair (either because they sent it, or because they overheard it)

### Opportunity Detection

The relay's forwarding queue is the natural place to detect coding opportunities. When a new packet enters the queue, scan for a partner:

```
on_packet_queued(pkt_new):
    for pkt_queued in forward_queue:
        if pkt_new.next_hop == pkt_queued.prev_hop AND
           pkt_queued.next_hop == pkt_new.prev_hop AND
           neighbor_has(pkt_new.next_hop, pkt_queued.packet_id) AND
           neighbor_has(pkt_queued.next_hop, pkt_new.packet_id):
            mark_coding_pair(pkt_new, pkt_queued)
            break
```

The simplest case: A→R→B and B→R→A. Alice sent pkt_17, so Alice has pkt_17. Bob sent pkt_23, so Bob has pkt_23. The relay knows both neighbors have the packet they originated. This case requires **zero reception reports** — it's free knowledge.

More complex cases (overheard packets from non-originators) require reception reports. Start with the simple bidirectional case.

### When Opportunities Arise

Coding opportunities require temporal overlap in the queue — two compatible packets must be queued at the same time. This depends on:

- **Traffic pattern:** Bidirectional conversation → high overlap. One-way sensor data → zero overlap.
- **Queue dwell time:** Packets sit in the queue during airtime budget waits, flow control backoff, or channel busy. Longer dwell = more overlap = more coding chances.
- **Packet rate:** Higher rate → more likely two packets coincide. But LoRa's low throughput means low packet rates.

---

## 3. Reception Reports

For the relay to code packets, it must know what its neighbors have seen. Three sources of knowledge, in order of cost:

### 3a. Implicit Knowledge (Free)

A node always has packets it originated. If pkt_17 was sent by Alice, Alice has pkt_17. This covers the primary bidirectional case with zero overhead.

### 3b. Piggybacked on ACKs (Low Cost)

Bramble ACKs already travel back to the sender. Add a small reception report:

```
ACK packet (existing fields):
  packet_id_acked: 2 bytes

Extended with reception report:
  report_count:    1 byte     (0-8 entries)
  recent_ids:      2 bytes × N  (packet_ids seen recently)
```

Overhead: 1 + 2N bytes per ACK. With N=4, that's 9 bytes — under 10% of a typical ACK packet. Acceptable.

Frequency: every ACK. Since ACKs happen per reliable packet, this keeps the relay's knowledge fresh with no additional transmissions.

### 3c. Piggybacked on Beacons (Low Cost)

If Bramble uses periodic beacons (route maintenance, neighbor discovery), piggyback the same reception report format. This covers cases where a node overhears packets but doesn't ACK them.

### 3d. Dedicated Reports (High Cost — Avoid)

A separate report packet defeats the purpose. The airtime cost of the report may exceed the savings from coding. **Do not implement dedicated reports.** If implicit knowledge + ACK piggybacking isn't enough, coding won't save airtime anyway.

### Reception Report Cache

Each node maintains a per-neighbor "heard set":

```c
typedef struct {
    uint8_t  neighbor_id;
    uint16_t heard_ids[CODING_HEARD_SET_SIZE];  // circular buffer
    uint8_t  head;
    uint8_t  count;
    uint32_t last_updated;  // tick
} neighbor_heard_t;

#define CODING_HEARD_SET_SIZE  16
#define CODING_MAX_NEIGHBORS    8
```

Total RAM: ~140 bytes per neighbor × 8 neighbors = ~1.1 KB. Fine for ESP32.

Entries expire after `CODING_HEARD_TTL` (default: 30 seconds). After that, we can't be confident the neighbor still has the packet cached.

---

## 4. Encoding

### Decision Logic

When the forwarding queue has a coding pair, the relay must decide: code or forward normally?

```
should_code(pkt_a, pkt_b):
    // Both receivers must be able to decode
    if NOT neighbor_has(pkt_a.next_hop, pkt_b.packet_id):
        return false
    if NOT neighbor_has(pkt_b.next_hop, pkt_a.packet_id):
        return false

    // Payloads must be similar size (padding waste)
    size_ratio = min(pkt_a.len, pkt_b.len) / max(pkt_a.len, pkt_b.len)
    if size_ratio < 0.5:
        return false  // too much padding waste

    // Don't code if either packet is already late
    if pkt_a.queue_age > MAX_CODING_DELAY OR pkt_b.queue_age > MAX_CODING_DELAY:
        return false

    return true
```

### XOR Encoding

Simple byte-wise XOR. Shorter payload is zero-padded to match the longer one:

```c
void encode_xor(const uint8_t *a, uint8_t len_a,
                const uint8_t *b, uint8_t len_b,
                uint8_t *out, uint8_t *out_len) {
    *out_len = MAX(len_a, len_b);
    memset(out, 0, *out_len);
    memcpy(out, a, len_a);
    for (uint8_t i = 0; i < len_b; i++) {
        out[i] ^= b[i];
    }
}
```

### 3+ Packet Coding

Theoretically possible (XOR three packets, each receiver needs the other two to decode). In practice over LoRa:

- **Finding 3-way coding opportunities is extremely rare** — requires three packets queued simultaneously with the right topology
- **Decoding requires more cached packets** — higher failure rate
- **Marginal savings over 2-way** — going from 2 transmissions to 1 saves 50%; going from 3 to 1 saves 67%, but the third opportunity almost never arises

**Recommendation: 2-packet XOR only.** Not worth the complexity for 3+.

### Coding Delay

To maximize coding opportunities, the relay could hold packets briefly hoping a partner arrives. This is the **coding delay** — a tradeoff between latency and coding rate.

For LoRa, recommend `MAX_CODING_DELAY = 0`. Don't hold packets. If a partner is already in the queue, great. If not, forward immediately. Rationale: LoRa latency is already high; adding deliberate delay for speculative coding is not worth it. Packets naturally accumulate during airtime budget waits anyway.

---

## 5. Decoding

### Receiver Logic

When a node receives a coded packet:

```
on_receive_coded(coded_pkt):
    for id in coded_pkt.component_ids:
        if id != my_expected_id:
            // This is the packet I need to XOR away
            cached = packet_cache_lookup(id)
            if cached:
                recovered = xor(coded_pkt.payload, cached.payload)
                deliver(recovered, my_expected_id)
                return
    // Can't decode — don't have the needed component
    request_retransmit(coded_pkt)
```

"My expected ID" is determined by checking which component_id was addressed to (or through) this node.

### Packet Cache

Every node must cache recent packets it has sent or received, so it can decode coded packets. This cache already partially exists for dedup — extend it to store payloads:

```c
typedef struct {
    uint16_t packet_id;
    uint8_t  payload[MAX_PAYLOAD_SIZE];
    uint8_t  payload_len;
    uint32_t timestamp;
} cached_packet_t;

#define CODING_CACHE_SIZE  16  // packets
```

RAM cost: 16 × (2 + 200 + 1 + 4) = ~3.3 KB. Acceptable on ESP32.

Cache eviction: LRU, with a TTL of 60 seconds. Any packet older than 60s is evicted — if a coded packet arrives needing it, decoding fails.

### Decode Failure

If the receiver can't decode (needed packet evicted or never seen):

1. Drop the coded packet (it's useless to this node)
2. The reliability layer's existing retransmit timer handles recovery — the sender will retransmit the original uncoded packet after ACK timeout
3. No special handling needed. The coding layer is opportunistic — failure degrades to normal behavior, not worse.

**Important:** A coding failure does NOT cause extra transmissions vs. not coding at all. The worst case is: we sent one coded packet instead of two uncoded ones, one receiver decoded successfully (net win), the other couldn't (net neutral after retransmit). Even partial success is a win.

---

## 6. Packet Format

### PKT_TYPE_CODED Header

```
Byte 0:     packet_type = PKT_TYPE_CODED (0x07)
Byte 1:     flags
              bit 0: reserved
              bit 1-2: num_components - 1 (0 = 2 packets, always 0 for now)
Byte 2-3:   coded_packet_id (unique ID for this coded packet, for dedup)
Byte 4-5:   component_id_0  (packet_id of first component)
Byte 6:     component_len_0 (original payload length of first component)
Byte 7-8:   component_id_1  (packet_id of second component)
Byte 9:     component_len_1 (original payload length of second component)
Byte 10+:   XOR'd payload
```

**Header overhead: 10 bytes** for a 2-packet coded packet.

### Overhead Analysis

Normal forwarding of 2 packets (100 bytes each, 8-byte header):
- 2 × (8 + 100) = 216 bytes transmitted
- 2 × ~480ms = ~960ms airtime

Coded forwarding:
- 1 × (10 + 100) = 110 bytes transmitted
- 1 × ~530ms airtime (slightly larger due to header)

**Savings: 106 bytes, ~430ms airtime (45% reduction)**

The 2-byte overhead per component ID is negligible. The fixed 10-byte header vs. 8-byte normal header costs 2 extra bytes — irrelevant compared to the 100+ bytes saved.

### Edge Case: Mismatched Payload Sizes

If pkt_a is 100 bytes and pkt_b is 30 bytes, the coded payload is 100 bytes (pkt_b zero-padded). The receiver of pkt_b uses `component_len_1 = 30` to know only the first 30 bytes are meaningful after XOR.

Efficiency drops with mismatched sizes: the coded packet is as large as the largest component. If one packet is 100 bytes and the other is 10 bytes, you transmit 110 bytes instead of 216 — still a win, but less dramatic (49% vs. 45%). The `size_ratio < 0.5` check in encoding filters out the worst cases.

---

## 7. Airtime Savings Analysis

### Model Parameters

- SF10/125kHz, 100-byte payload: ~480ms airtime
- Header overhead for coded packet: +2 bytes → ~490ms
- 10% duty cycle budget: 360ms per 3.6 seconds

### Scenario: Bidirectional Chat (Best Case)

Two nodes exchanging messages through one relay, roughly symmetric rate.

- Without coding: 2 forwards per exchange × 480ms = 960ms
- With coding: 1 coded broadcast × 490ms = 490ms
- **Savings: 49% airtime at relay**
- Relay's airtime budget effectively doubles for this traffic

This is the sweet spot. Real-time chat between two field teams through a hilltop relay.

### Scenario: Mostly Unidirectional (Worst Case)

Sensor node sending telemetry to a gateway. Traffic is 95% one direction.

- Coding opportunities: ~5% of packets (only when ACKs or rare reverse traffic coincides)
- Effective savings: <3%
- Reception report overhead in ACKs: ~5 bytes per ACK
- **Net savings: roughly zero or slightly negative**

Not worth enabling for unidirectional flows.

### Scenario: Multi-Node Mesh (Moderate)

4 nodes, various conversations, 2 relay nodes.

- Coding opportunities depend on traffic matrix overlap at relays
- Estimate: 10-30% of forwarded packets are codable
- **Effective savings: 5-15% overall airtime**

Moderate, but meaningful when airtime budget is tight.

### When Coding Is NOT Worth It

- **Unidirectional traffic** — no coding opportunities
- **Very low traffic rate** — packets don't overlap in the queue, no coding pairs form
- **Single-hop networks** — no relay, no forwarding, no coding
- **High packet loss** — coded packets that can't be decoded waste more airtime than they save
- **Asymmetric payload sizes** — large packet + tiny packet → minimal savings

### Break-Even Analysis

The overhead of reception reports (~5 bytes per ACK) costs additional airtime. For coding to break even, we need roughly 1 successful coding event per 20 ACKs sent. In a bidirectional flow, this is easily met. In a mostly-unidirectional flow, it may not be.

**Recommendation: Enable coding per-route, not globally.** If a route has seen bidirectional traffic recently, enable coding for that relay. Otherwise, skip the reception report overhead.

---

## 8. Interaction with Existing Systems

### Reliability / ACKs

- A coded packet is **not** a replacement for ACKs. The reliability layer still expects an ACK for each component packet.
- When receiver B decodes pkt_17 from the coded packet, it ACKs pkt_17 normally back toward A.
- The relay can treat the coded transmission as having forwarded both components — it stops both retransmit timers.
- If one receiver fails to decode: the other receiver's ACK flows back normally. The failed receiver's ACK never arrives, triggering retransmit of the original uncoded packet. Self-healing.

### Airtime Budget

- The relay spent ~490ms sending one coded packet instead of ~960ms for two uncoded.
- Credit the budget for the saved transmission: `airtime_credit(480ms)` — not literally, but the token bucket naturally refills faster since fewer tokens were spent.
- No special accounting needed. The savings are implicit.

### Dedup

- Coded packets get their own `coded_packet_id` for dedup at the link layer.
- After decoding, the recovered packet has its original `packet_id` — dedup at the routing layer uses this as normal.
- No conflicts. The coded packet and its components are different packet types with different IDs.

### Fragmentation

- **Do not code fragments.** A coded fragment is useless to a receiver who doesn't have the exact corresponding fragment from the other packet.
- If a packet is fragmented, each fragment is forwarded normally. Coding only applies to complete, unfragmented packets.
- Alternative: code reassembled packets only. But this means the relay must reassemble before coding, adding RAM and complexity. Not worth it.
- **Rule: coding operates on unfragmented packets only.** If `pkt.is_fragment`, skip coding.

---

## 9. Complexity Assessment

### Honest Evaluation

**COPE was designed for a different world.** WiFi mesh networks have:
- ~100 Mbps throughput → deep queues → many coding opportunities
- ~1ms packet times → low cost of holding packets for coding
- Dense traffic → high probability of bidirectional flows

LoRa mesh has:
- ~1-5 Kbps effective throughput → shallow queues → few coding opportunities
- ~500ms packet times → high cost of coding delay
- Sparse traffic → bidirectional flows are less common

### When This Actually Helps

The narrow-but-real sweet spot for Bramble:

1. **Hilltop relay between two teams** — bidirectional chat messages through a single relay. Classic Alice-and-Bob. Coding cuts relay airtime nearly in half.
2. **Request-response patterns** — node A queries node B, B responds. If both pass through the same relay and overlap in the queue, coding applies.
3. **Duty-cycle-limited relays** — when a relay is near its 10% duty cycle limit, coding extends the budget. Even a 20% coding rate meaningfully extends the relay's capacity.

### When This Doesn't Help

1. Sensor networks (unidirectional telemetry)
2. Low-traffic networks (packets don't overlap in queue)
3. Single-hop deployments
4. Networks with high packet loss (decode failures waste airtime)

### Verdict

**Implement as a low-priority optimization, gated behind a config flag.** The core coding logic is simple (~300 lines of C). The reception reports piggybacked on ACKs add minimal overhead. The packet cache extends existing dedup infrastructure.

Risk is low — coding is opportunistic, so when it doesn't fire, there's near-zero cost (just the reception report bytes in ACKs). When it does fire, the savings are meaningful.

Don't implement coding delay (holding packets). Don't implement 3+ packet coding. Keep it simple.

**Estimated effort: 2-3 days for core implementation, 1 day for simulator scenarios.**

---

## 10. Integration

### New Component: `components/coding/`

```
components/coding/
├── coding.h          // Public API
├── coding.c          // Encoding/decoding, opportunity detection
├── coding_cache.h    // Packet cache for decoding
├── coding_cache.c
├── reception_report.h // Neighbor heard-set tracking
├── reception_report.c
└── test/
    ├── test_coding.c
    ├── test_cache.c
    └── test_report.c
```

### API

```c
// Initialize coding subsystem
void coding_init(void);

// Called when a packet enters the forwarding queue
// Returns a coding partner if found, NULL otherwise
packet_t *coding_find_partner(const packet_t *pkt);

// Encode two packets into a coded packet
// Returns coded packet (caller frees), or NULL on failure
packet_t *coding_encode(const packet_t *a, const packet_t *b);

// Attempt to decode a received coded packet
// Returns decoded packet if successful, NULL if missing cache entry
packet_t *coding_decode(const packet_t *coded);

// Update neighbor heard-set from a reception report
void coding_report_received(uint8_t neighbor_id,
                            const uint16_t *ids, uint8_t count);

// Generate reception report for piggybacking on outgoing ACK
uint8_t coding_generate_report(uint16_t *ids_out, uint8_t max_ids);
```

### Modifications to Existing Code

**`components/forwarding/forwarding.c`:**
- In `forward_queue_process()`: before transmitting, call `coding_find_partner()`. If found, encode and broadcast the coded packet. Remove both components from the queue.
- ~20 lines of changes.

**`components/reliability/ack.c`:**
- In `ack_build()`: call `coding_generate_report()` and append to ACK payload.
- In `ack_receive()`: extract reception report, call `coding_report_received()`.
- ~15 lines of changes.

**`components/packet/packet.h`:**
- Add `PKT_TYPE_CODED = 0x07`
- Add coded packet header struct.
- ~10 lines.

**`components/routing/routing.c`:**
- In `route_receive()`: handle `PKT_TYPE_CODED` — call `coding_decode()`, then process recovered packet normally.
- ~10 lines.

### Configuration

```c
#define CONFIG_CODING_ENABLED        1     // Master enable
#define CONFIG_CODING_CACHE_SIZE     16    // Packets cached for decoding
#define CONFIG_CODING_CACHE_TTL_S    60    // Cache entry lifetime
#define CONFIG_CODING_HEARD_SET_SIZE 16    // Per-neighbor heard IDs
#define CONFIG_CODING_HEARD_TTL_S    30    // Heard-set entry lifetime
#define CONFIG_CODING_MAX_NEIGHBORS  8     // Max tracked neighbors
#define CONFIG_CODING_MIN_SIZE_RATIO 0.5   // Min payload size ratio to code
#define CONFIG_CODING_DELAY_MS       0     // Don't hold packets (0 = disabled)
```

---

## 11. Simulator Scenarios

### Scenario A: Bidirectional Chat Through Relay

```
Topology: A --- R --- B (linear, 3 nodes)
Traffic:  A→B: 1 msg/30s, B→A: 1 msg/30s (alternating)
Duration: 10 minutes
Measure:  relay airtime with/without coding, delivery ratio, latency
Expected: ~40-50% airtime savings at relay
```

### Scenario B: Asymmetric Traffic

```
Topology: A --- R --- B
Traffic:  A→B: 1 msg/10s, B→A: 1 msg/60s
Duration: 10 minutes
Measure:  coding rate, actual airtime savings
Expected: ~15-20% of A→B packets get coded (when B→A coincides)
```

### Scenario C: Multi-Hop Chain

```
Topology: A --- R1 --- R2 --- B
Traffic:  A→B and B→A, 1 msg/30s each direction
Measure:  coding at R1, coding at R2, end-to-end savings
Expected: coding at both relays, ~40% savings at each
```

### Scenario D: Star Topology (Coding Across Flows)

```
Topology: A, B, C all connected to relay R
Traffic:  A→B and B→C (different flows)
Measure:  can R code A→B with B→C? (only if A has B→C somehow)
Expected: minimal coding — flows don't share components
```

### Scenario E: Unidirectional Sensor (Negative Case)

```
Topology: Sensor S --- R --- Gateway G
Traffic:  S→G: 1 msg/60s, G→S: ACK only
Measure:  coding opportunities, overhead of reception reports
Expected: ~0 coding, small overhead from report bytes in ACKs
Validates: coding doesn't hurt when not applicable
```

---

## Task Breakdown

### Phase 1: Core (2 days)

- [ ] **T1.1** Implement `coding_cache.c` — packet cache with LRU eviction and TTL
- [ ] **T1.2** Implement `reception_report.c` — per-neighbor heard-set tracking
- [ ] **T1.3** Implement `coding.c` — XOR encode/decode, opportunity detection
- [ ] **T1.4** Unit tests for all three modules

### Phase 2: Integration (1 day)

- [ ] **T2.1** Add `PKT_TYPE_CODED` to packet header definitions
- [ ] **T2.2** Hook coding into forwarding queue (`forwarding.c`)
- [ ] **T2.3** Piggyback reception reports on ACKs (`ack.c`)
- [ ] **T2.4** Handle coded packet reception in routing (`routing.c`)
- [ ] **T2.5** Add config flags, default to disabled

### Phase 3: Simulator (1 day)

- [ ] **T3.1** Implement scenarios A-E in simulator
- [ ] **T3.2** Add airtime tracking metrics for coded vs. uncoded packets
- [ ] **T3.3** Run scenarios, document results
- [ ] **T3.4** Determine default on/off recommendation based on results

### Phase 4: Polish (0.5 days)

- [ ] **T4.1** Add `coding_stats` to diagnostics (coding rate, decode failures, airtime saved)
- [ ] **T4.2** Runtime enable/disable via config command
- [ ] **T4.3** Documentation in protocol spec

**Total estimate: 4-5 days**
