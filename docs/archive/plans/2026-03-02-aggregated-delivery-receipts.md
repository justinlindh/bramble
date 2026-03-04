# Aggregated Delivery Receipt Design

## Motivation

Bramble's per-node broadcast delivery receipts are a key differentiator — knowing exactly *who* received your message provides real trust and situational awareness that no other LoRa mesh offers. However, the current design where every recipient independently sends a receipt back to the sender creates O(N) receipt traffic that doesn't scale past ~20 nodes.

**Goal:** Preserve full per-node delivery visibility while reducing receipt airtime from O(N × H) to approximately O(relay_nodes × H) by aggregating receipts along the natural broadcast tree.

## How Broadcasts Propagate — The Implicit Tree

When a node sends a broadcast, it propagates outward through the mesh as a tree:

```
         Sender (A)
        /    |    \
      B      C      D          ← 1-hop neighbors (direct)
     / \     |     / \
    E   F    G    H   I        ← 2-hop nodes
    |        |
    J        K                 ← 3-hop nodes
```

Each node receives the broadcast from exactly one "parent" — the node that relayed it. This is determined by which copy arrives first (subsequent copies are dedupped).

**Key insight:** This tree already exists implicitly. We can exploit it for receipt aggregation without any additional discovery overhead.

## Current Receipt Flow (O(N) packets)

```
1. A broadcasts message
2. B, C, D receive directly → each sends independent receipt to A
3. E, F, G, H, I receive via relay → each sends independent receipt to A (routed)
4. J, K receive via 2 relays → each sends independent receipt to A (routed)

Total: 10 independent receipt packets, each routed back to A
With 3 retries each: 30 transmissions minimum
With multi-hop forwarding: 30 × avg_hops ≈ 60-90 transmissions
```

## Proposed Aggregated Receipt Flow (O(tree_branches) packets)

```
1. A broadcasts message
2. B, C, D receive directly → start aggregation windows
3. E→B: "I received it" (1-hop receipt to parent)
4. F→B: "I received it" (1-hop receipt to parent)
5. J→E→B: "J received it" (relayed to B via E, E adds itself)
6. B's aggregation window expires →
   B sends ONE aggregated receipt to A: [B, E, F, J]
7. Similarly: C sends [C, G, K], D sends [D, H, I]

Total: 3 aggregated receipt packets to sender (+ ~7 short-range child reports)
Versus 10 independent long-range receipts in current design
```

## Protocol Design

### New Packet Type: AGGREGATED_DELIVERY_RECEIPT (0x15)

```
Offset  Size  Field              Description
──────  ────  ─────              ───────────
0       12    header             Common header (type=0x15, dest=original_sender)
12      4     aggregator_addr    Node that built this aggregated receipt
16      4     orig_packet_id     Packet ID of the original broadcast
20      1     recipient_count    Number of recipient addresses (1-25)
21      1     flags              Aggregation flags (see below)
22      N×4   recipients         Array of addresses that received the broadcast
──────────────────────────────────────────────────────────────────
Total: 22 + (recipient_count × 4) bytes
Max: 22 + 100 = 122 bytes (25 recipients)
```

**Flags:**
- Bit 0: `AGG_FLAG_PARTIAL` — more receipts may follow (aggregation window was cut short)
- Bit 1: `AGG_FLAG_INCLUDES_SELF` — aggregator_addr is also a recipient (always set for normal operation)
- Bits 2-4: `AGG_FLAG_DEPTH` — max depth of children included (0=self only, 1=direct children, etc.)
- Bits 5-7: reserved

**Note on packet size:** At SF9/BW125, our effective MTU is ~203 bytes. With 22 bytes overhead, we can fit ~45 recipient addresses per packet. For meshes >45 nodes per branch, the aggregator fragments across multiple packets or sends partial aggregations.

### Child Receipt Report (lightweight, 1-hop only)

Instead of creating a new packet type for child→parent reports, we reuse the existing DELIVERY_RECEIPT (0x07) with `hop_limit=1`. This means:
- Child sends a standard delivery receipt but with `hop_limit=1`
- The parent receives it, records the child address, but does NOT forward it
- Instead, the child's address gets included in the parent's aggregated receipt

This avoids a new packet type for the child report and reuses existing serialization.

### Receipt Flow State Machine (per-node, per-broadcast)

```
                          ┌─────────────────┐
  Receive broadcast ──→   │ COLLECTING       │
                          │ (aggregation     │
                          │  window open)    │
                          └────────┬────────┘
                                   │
                    Window expires  │  Child receipts
                    OR capacity hit │  arrive and are
                                   │  added to list
                                   ▼
                          ┌─────────────────┐
                          │ SEND_AGGREGATE   │
                          │ (build + queue)  │
                          └────────┬────────┘
                                   │
                                   ▼
                          ┌─────────────────┐
                          │ DONE             │
                          └─────────────────┘
```

### Aggregation Window Timing

The aggregation window must be long enough for children to send their receipts but short enough to not delay the sender's view excessively.

**Proposed timing:**
- **Direct neighbors (1-hop):** 3-second aggregation window
  - Rationale: children use existing slot timing (up to 6.4s), but most arrive within 2-3s
- **2+ hop nodes:** 5-second aggregation window
  - Rationale: need to receive aggregated receipts from their own children first
- **Leaf nodes (no children after window):** send standard receipt immediately (no aggregation needed)
  - Optimization: if a node hasn't heard any child receipts within 1s, assume it's a leaf and send immediately

### How a Node Knows It's a "Parent"

A node knows it's a relay/parent if it **rebroadcasted** the message. In Bramble, the mesh task already tracks this via the dedup cache — if we forwarded/rebroadcasted a packet, we know downstream nodes may have received it from us.

**Simple heuristic:** If `hop_limit` was decremented and the packet was rebroadcasted → open an aggregation window. Otherwise → leaf behavior (send receipt directly).

### Determining "My Parent"

When a node receives a broadcast, it doesn't explicitly know which node relayed it to them (LoRa packets don't carry a "last hop" field — they only have the original source). 

**Solution options:**
1. **Use RSSI/timing correlation with neighbor table** — the node we received it from is likely a known neighbor; match the relayer by RSSI signature
2. **Add a `prev_hop` field to broadcast packets** — 4 bytes, simple and deterministic
3. **Send child receipt to original sender with hop_limit=1** — the receipt naturally reaches only direct neighbors; whichever one is on the path back to the sender will be the aggregator
4. **Don't require parent knowledge** — children send their receipt as hop_limit=1 broadcast; any relay neighbor that forwarded the original broadcast will aggregate it

**Recommended: Option 4** — It's the simplest. The child sends a hop_limit=1 delivery receipt (effectively a local advertisement: "I received this"). Any neighbor that acted as a relay for that broadcast picks it up and includes the child in its aggregation. If multiple neighbors relayed it, the child might be counted by more than one aggregator — the sender deduplicates by address.

### Sender-Side Deduplication

The sender maintains a set of addresses per broadcast ID. As aggregated receipts arrive:
1. Deserialize recipient list
2. Add each address to the set (dedup)
3. Update the delivery event ring for UI/RPC notification
4. Emit `bramble.onBroadcastDelivery` events as batches arrive

The sender doesn't need to know the tree topology — it just collects addresses.

## Backward Compatibility

- Old firmware nodes will continue sending individual receipts (type 0x07) — the sender handles both
- New firmware nodes receiving individual receipts from old nodes will aggregate them normally
- The aggregated receipt (type 0x15) will be ignored by old firmware (unknown packet type → dropped)
- Transition period: sender sees a mix of individual and aggregated receipts, dedup handles it

## Airtime Analysis

### Current (N=100, 5 hops avg)
- 99 independent receipts × 3 retries × ~200ms × avg 4.5 hops ≈ **267 seconds** mesh airtime

### Aggregated (N=100, 5 hops avg, ~20 relay branches)
- 99 child reports at 1-hop (no retries needed at 1-hop range): 99 × 200ms = **19.8s**
- 20 aggregated receipts traveling ~3 hops avg: 20 × 2 retries × 200ms × 3 = **24s**
- Total: **~44 seconds** — an **84% reduction**

### Aggregated (N=20, 3 hops avg, ~8 relay branches)
- 19 child reports at 1-hop: 19 × 200ms = **3.8s**
- 8 aggregated receipts × 2 hops avg: 8 × 2 retries × 200ms × 2 = **6.4s**
- Total: **~10s** vs current **~29s** — a **65% reduction**

## Implementation Phases

### Phase 1: Aggregation infrastructure (firmware)
- Add `AGGREGATED_DELIVERY_RECEIPT` packet type and serialization
- Add per-broadcast aggregation state (collecting window, recipient list)
- Modify `queue_broadcast_delivery_receipt()` to use hop_limit=1 for non-direct broadcasts
- Add aggregation window timer (reuse the existing receipt timer infrastructure)
- Sender-side: handle both individual and aggregated receipts, dedup by address

### Phase 2: Sender UX
- Update delivery event ring to support batch receipt events
- Update RPC `bramble.onBroadcastDelivery` to emit aggregated events
- Update TUI/webapp delivery display to show receipts arriving in batches

### Phase 3: Simulator support
- Model broadcast tree propagation
- Model child→parent 1-hop receipts
- Model aggregation windows and aggregated receipt packets
- Add metrics: receipt_airtime_savings, aggregation_ratio, receipt_latency_p50/p95
- Run at N=10, 50, 100, 200 to validate airtime projections

### Phase 4: Tuning and edge cases
- Optimize aggregation window timing based on real-world data
- Handle node mobility (parent changes between broadcasts)
- Handle partial network partition (some children unreachable)
- Consider probabilistic child reporting for very dense meshes (>50 neighbors)

## Data Structures

### Aggregation State (per pending broadcast, on relay nodes)

```c
#define AGG_MAX_CHILDREN     32
#define AGG_WINDOW_DIRECT_MS 3000
#define AGG_WINDOW_RELAY_MS  5000

typedef struct {
    bool active;
    uint32_t orig_packet_id;
    uint32_t original_src_addr;  /* broadcast sender */
    uint32_t recipients[AGG_MAX_CHILDREN];
    uint8_t  recipient_count;
    uint32_t window_expires_ms;
    bool     i_am_relay;         /* did I rebroadcast this? */
} broadcast_agg_state_t;

#define AGG_SLOTS 4  /* max concurrent broadcast aggregations */
static broadcast_agg_state_t s_agg_slots[AGG_SLOTS];
```

### Memory Impact
- 4 slots × (1 + 4 + 4 + 128 + 1 + 4 + 1) = 4 × 143 = **572 bytes**
- Negligible compared to existing mesh state (~8KB)

## Open Questions

1. **Should aggregated receipts include signal quality?** Adding RSSI per-recipient would double the per-address cost (4→8 bytes) but provide valuable mesh intelligence. Could be optional via flag.

2. **Max aggregation depth?** Should a relay aggregate its children's children, or only direct children? Deeper aggregation = fewer packets but longer windows and more complexity. Recommend starting with depth=1 (only direct children) and iterating.

3. **Receipt priority vs data priority?** With budget enforcement, should aggregated receipts use a dedicated airtime tier or share broadcast tier? Dedicated tier prevents receipt starvation of normal broadcast traffic.

4. **Retry strategy for aggregated receipts?** These are more valuable than individual receipts (they carry multiple addresses). Worth 2-3 retries with the existing exponential backoff. Loss of one aggregated receipt means losing visibility on multiple nodes.

## Success Criteria

- Sender receives the same per-node delivery information as today
- Airtime consumed by receipts scales sub-linearly with node count
- Simulator validates >60% airtime reduction at N=50+
- No regression in receipt completeness for small meshes (N<20)
- Backward compatible with current firmware
