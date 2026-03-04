# Simulator Component Integration Plan

> ✅ **ALL PHASES COMPLETE**

> Close the gaps between the simulator and the real Bramble protocol stack.

**Goal:** Wire in the missing firmware components (reliability, dedup, crypto, airtime, fragment) so the simulator tests the actual protocol pipeline, not a simplified approximation. Add scenarios that exercise each component and a "success on retry" metric.

**Constraint:** Zero modifications to component source code. Simulator `#include`s `.c` files directly and bridges via `bridge.c`.

---

## Current State

**Included:** `packet`, `routing` (discovery, forwarding, routing tables)
**Missing:** `reliability`, `dedup`, `fragment`, `airtime`, `crypto`
**Not applicable to sim:** `ble`, `display`, `ota`, `ui`, `identity` (hardware/interface), `timesync` (no real clocks), `channel` (future), `security` (needs key exchange flow)

## Phase 1: Reliability — ACKs & Delivery Receipts

**Why first:** This is the biggest functional gap and the differentiating feature (path tracing). Without it, undelivered messages are invisible.

### Task 1.1: Add reliability state to sim_node_t

Add to `sim_node.h`:
```c
#include "../../components/reliability/include/reliability.h"
pending_ack_table_t pending_acks;
flow_control_t flow_control;
```

### Task 1.2: Include reliability.c in all.c

Add `#include "../../components/reliability/reliability.c"` to `simulator/gosim/all.c`.

### Task 1.3: Wire ACK into receive pipeline

In `bridge_handle_receive_packet()`:
- When a DATA packet arrives at its final destination:
  - Build a `PKT_TYPE_DELIVERY_RECEIPT` with `relay_path` recording each hop
  - Send the receipt back toward the source via existing routes
  - Emit `message_delivered` with the full path
- When an ACK arrives:
  - Call `pending_ack_remove()` to clear the pending entry
  - Call `flow_on_ack()` to open the flow window

### Task 1.4: Wire ACK into send pipeline

In `bridge_handle_generate_message()`:
- After sending a DATA packet, add to `pending_ack_add()` with tier and retry info
- Check `flow_can_send()` before sending — if window full, reschedule
- On `pending_ack_tick()` timeout: retransmit or give up based on tier

### Task 1.5: Path tracking

- Add a `relay_path` accumulator that each forwarding node appends its address to
- On delivery receipt: emit the full path in the `message_delivered` event
- UI: animate the delivery receipt path as a distinct visual (green trace back to sender)

### Task 1.6: "Success on retry" metric

Add to `metrics_state_t`:
```c
uint64_t messages_retried;        // messages that needed ACK retransmit
uint64_t messages_delivered_retry; // messages delivered after ≥1 retransmit
```

Bridge logic:
- Increment `messages_retried` when `pending_ack_tick()` triggers a retransmit
- Increment `messages_delivered_retry` when a delivery receipt arrives for a packet that had `attempt > 1`

Emit in metrics ticks and final_metrics as `retried` and `delivered_on_retry`.

### Task 1.7: Scenario — `reliability-ack-retry.json`

10-node grid. Intermittent link failures (move nodes in/out of range) while sending messages at tier NORMAL and CRITICAL. Expected:
- NORMAL tier: 3 retries, gives up after ~6s
- CRITICAL tier: 8 retries with longer backoff, gives up after ~120s
- `delivered_on_retry` > 0 (some messages succeed after initial failure)
- `undelivered` count for messages where all retries failed

### Task 1.8: Scenario — `reliability-path-trace.json`

Linear 5-node chain A→B→C→D→E. Send messages A→E. Verify delivery receipts contain `relay_path: [B, C, D]`. Move C out of range. Verify A→E messages reroute (if alternate path exists) or get properly dropped with retry exhaustion.

---

## Phase 2: Dedup — Real Duplicate Detection

**Why:** The sim currently has a basic dedup hack in the radio layer. The real `dedup.c` component is small (54 lines) and tested.

### Task 2.1: Add dedup state to sim_node_t

```c
#include "../../components/dedup/include/dedup.h"
dedup_buffer_t dedup;
```

### Task 2.2: Include dedup.c in all.c

### Task 2.3: Wire into receive pipeline

In `bridge_handle_receive_packet()`:
- Before processing any received packet, call `dedup_check_and_add()`
- If duplicate, drop and emit `packet_dropped` with `reason: "duplicate"`
- Call `dedup_purge()` periodically in node_tick

### Task 2.4: Remove sim's ad-hoc dedup

Replace any sim-level dedup with the real component.

### Task 2.5: Scenario — `dedup-flood.json`

Dense 8-node mesh (all within range). Send broadcast messages. Verify dedup prevents duplicate processing. Metric: packets_dropped with reason "duplicate" should be > 0.

---

## Phase 3: Airtime Budget

**Why:** Catches duty cycle violations before field testing. LoRa regulations require <1% duty cycle in most regions.

### Task 3.1: Add airtime state to sim_node_t

```c
#include "../../components/airtime/include/airtime_budget.h"
airtime_budget_t airtime;
```

### Task 3.2: Include airtime_budget.c in all.c

### Task 3.3: Wire into send pipeline

Before any `sim_radio_broadcast()`:
- Estimate airtime based on packet size and LoRa params (SF, BW, CR)
- Call `airtime_budget_can_transmit()` — if false, defer the packet
- After send, call `airtime_budget_debit()`
- Call `airtime_budget_refill()` periodically in node_tick

### Task 3.4: Emit airtime events

```json
{"type": "airtime_exceeded", "node": "A", "tier": 1, "remaining_ms": 0}
```

### Task 3.5: Scenario — `airtime-exhaustion.json`

3-node chain with aggressive message sending (1 msg/second). After ~18s of transmission, normal tier budget exhausts. Verify:
- Messages get deferred (not dropped) when budget empty
- Budget refills after 1 hour (accelerated in sim)
- Anomaly: node can't participate in routing while airtime-limited

---

## Phase 4: Fragment — Large Message Support

**Why:** Real messages may exceed the ~154 byte LoRa payload. Fragment/reassembly is critical for usability.

### Task 4.1: Add fragment state to sim_node_t

```c
#include "../../components/fragment/include/fragment.h"
reassembly_ctx_t reassembly;
```

### Task 4.2: Include fragment.c in all.c

### Task 4.3: Wire into send/receive pipeline

Send side (in generate_message):
- If message payload > FRAG_MAX_PLAINTEXT, call `fragment_split()`
- Send each fragment as a separate DATA packet with frag_header

Receive side:
- On DATA with frag_header, call `reassembly_add()`
- When all fragments received, `reassembly_collect()` and deliver
- Call `reassembly_purge()` periodically

### Task 4.4: Scenario — `fragment-large-message.json`

5-node chain. Send messages of varying sizes (50, 150, 300, 600 bytes). Verify:
- Small messages (≤154): single packet, delivered normally
- Large messages: split into 2-4 fragments, all fragments delivered, message reassembled
- Fragment loss: one fragment dropped → reassembly timeout → message_dropped

---

## Phase 5: Crypto — Realistic Packet Sizes

**Why:** Encryption adds overhead (nonce + tag = 28 bytes). Packet size affects airtime, fragmentation thresholds, and radio layer behavior.

### Task 5.1: Use crypto_host.c (POSIX mbedtls)

`crypto_host.c` is the host-compatible implementation. Include it in all.c.

### Task 5.2: Add crypto state to sim_node_t

```c
#include "../../components/crypto/include/crypto.h"
bramble_identity_t identity;
uint8_t session_keys[MAX_NODES][BRAMBLE_KEY_SIZE]; // pre-shared for sim
```

### Task 5.3: Wire into send/receive

Send: encrypt DATA payload with AES-256-GCM before transmission
Receive: decrypt, verify tag, drop if authentication fails

### Task 5.4: Pre-shared keys in scenarios

For sim simplicity: derive pairwise keys from node addresses at startup (deterministic). No key exchange handshake needed — that's a `security` component concern.

### Task 5.5: Scenario — `crypto-overhead.json`

Same as ideal-10-node but with encryption enabled. Compare delivery rates and latency with unencrypted baseline. Verify:
- Packet sizes increase by ~28 bytes
- Fragmentation triggers at lower message sizes
- Delivery rates remain ≥95%

---

## New Metrics Summary

| Metric | Description |
|--------|-------------|
| `retried` | Messages that triggered ≥1 ACK retransmission |
| `delivered_on_retry` | Messages delivered after retry (success on retry) |
| `undelivered` | Sent but never confirmed delivered |
| `dedup_dropped` | Packets dropped as duplicates |
| `airtime_deferred` | Packets deferred due to duty cycle budget |
| `fragments_sent` | Total fragment packets sent |
| `fragments_reassembled` | Successfully reassembled messages |
| `reassembly_timeout` | Messages lost to incomplete fragment sets |

---

## Scenario Summary

| Scenario | Tests | Key Metric |
|----------|-------|------------|
| `reliability-ack-retry.json` | ACK retransmission under link failures | `delivered_on_retry` > 0 |
| `reliability-path-trace.json` | Delivery receipt with relay path | Path correctness |
| `dedup-flood.json` | Duplicate detection in dense mesh | `dedup_dropped` > 0 |
| `airtime-exhaustion.json` | Duty cycle enforcement | `airtime_deferred` > 0 |
| `fragment-large-message.json` | Fragmentation and reassembly | `fragments_reassembled` > 0 |
| `crypto-overhead.json` | Encryption impact on performance | Delivery rate comparison |

---

## Implementation Order

Phases are independent but ordered by impact:
1. **Reliability** (most impactful — ACKs, path tracing, retry metrics)
2. **Dedup** (small, easy, improves accuracy)
3. **Airtime** (regulatory compliance validation)
4. **Fragment** (large message support)
5. **Crypto** (packet size realism, depends on mbedtls availability in build)

Estimated effort: ~3-4 sessions for all 5 phases. Phase 1 (reliability) is the largest at ~60% of the work.
