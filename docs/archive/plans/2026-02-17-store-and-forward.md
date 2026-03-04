# Store-and-Forward for Offline Nodes

> ✅ **SIMULATOR IMPLEMENTATION COMPLETE** (2026-02-17) — Checkboxes below not updated but all tasks were implemented in the simulator branch.

> Let messages survive destination outages. Mailbox nodes buffer E2E-encrypted messages and deliver them when the destination rejoins the mesh.

**Date:** 2026-02-17
**Status:** Draft
**Components:** `routing`, `reliability`, `packet`, new `mailbox`

---

## Problem

If a Bramble node is offline (sleeping, out of range, powered off), any message routed to it fails after RREQ retries exhaust. The sender gets a RERR. The message is gone. Neither Meshtastic nor MeshCore solves this — both treat the mesh as ephemeral.

Bramble can do better. The mesh already knows which nodes are present (beacons, 30s interval, 60s neighbor purge). We can designate certain nodes as **mailbox nodes** that accept and buffer messages for offline destinations, then deliver them when the destination reappears.

---

## 1. Mailbox Node Designation

### Config

New Kconfig option:

```
CONFIG_BRAMBLE_MAILBOX_ENABLED=y       # Act as a mailbox node
CONFIG_BRAMBLE_MAILBOX_MAX_KB=8        # RAM budget for stored messages (default 8KB)
```

### Beacon Advertisement

Reserve bit 0 of `bramble_beacon_t.flags` as `BEACON_FLAG_MAILBOX`:

```c
#define BEACON_FLAG_MAILBOX  (1 << 0)
```

When `CONFIG_BRAMBLE_MAILBOX_ENABLED=y`, the node sets this bit in every beacon. Neighbors record it in their neighbor table entry:

```c
typedef struct {
    /* existing fields... */
    bool is_mailbox;    /* learned from beacon flags */
} neighbor_entry_t;
```

### Discovery

No dedicated mailbox discovery protocol. Mailbox capability is learned passively via beacons — the same way neighbors are already discovered. Since beacons are single-hop, only direct neighbors know about mailbox capability. This is fine: the mailbox needs to be reachable by the sender or an intermediate forwarder, and the destination needs to reach it on rejoin.

---

## 2. Message Buffering

### Trigger

When a node attempts to send (or forward) a DATA packet and:
1. Route discovery fails (RREQ times out with no RREP), **or**
2. A RERR is received indicating the next hop to the destination is broken

...the node checks its neighbor table for any neighbor with `is_mailbox == true`. If found, it sends a `STORE_REQUEST` to the nearest mailbox node.

If no mailbox neighbor exists, the message is dropped as today (RERR back to source).

### Store Request Flow

```
Sender ──STORE_REQUEST──▶ Mailbox
Sender ◀──STORE_ACK──── Mailbox
```

The sender picks the mailbox neighbor with the best link quality (lowest hop count to sender, highest beacon RSSI). If multiple mailbox nodes are equidistant, prefer the one with lower `tx_queue_depth`.

### Buffer Data Structure

```c
#define MAILBOX_MAX_MESSAGES     32
#define MAILBOX_MAX_PER_DEST     8
#define MAILBOX_MAX_PER_SOURCE   8
#define MAILBOX_MSG_MAX_SIZE     222   /* matches pending_ack packet_data size */
#define MAILBOX_DEFAULT_TTL_S    3600  /* 1 hour */

typedef struct {
    uint32_t dest_addr;
    uint32_t src_addr;
    uint32_t packet_id;
    uint32_t store_time_ms;     /* when buffered (monotonic clock) */
    uint32_t ttl_ms;            /* expiry relative to store_time */
    uint16_t packet_len;
    uint8_t  packet_data[MAILBOX_MSG_MAX_SIZE];
    bool     active;
} mailbox_entry_t;

typedef struct {
    mailbox_entry_t entries[MAILBOX_MAX_MESSAGES];
    uint8_t         count;
} mailbox_t;
```

### Eviction Policy

When the buffer is full and a new `STORE_REQUEST` arrives:

1. **Expired messages** — evict any entry where `now_ms - store_time_ms > ttl_ms`. Check on every store and every 30s tick.
2. **Per-destination cap** — if the destination already has `MAILBOX_MAX_PER_DEST` messages buffered, evict the oldest for that destination (FIFO).
3. **Per-source cap** — if a single source has `MAILBOX_MAX_PER_SOURCE` messages buffered, evict the oldest from that source. Prevents a single chatty node from monopolizing the buffer.
4. **Global full** — if still full after (1–3), evict the globally oldest entry.

### NACK on Reject

If the mailbox cannot accept (e.g., buffer truly exhausted and policy won't evict), it responds with `STORE_NACK` instead of `STORE_ACK`. The sender can try another mailbox neighbor or drop the message.

---

## 3. Delivery on Rejoin

### Detection

Mailbox nodes already receive beacons from all single-hop neighbors. When a beacon arrives from a node address that matches any `dest_addr` in the mailbox buffer, delivery is triggered.

```c
void mailbox_on_beacon(mailbox_t *mb, uint32_t beacon_src_addr, uint32_t now_ms) {
    for (int i = 0; i < MAILBOX_MAX_MESSAGES; i++) {
        if (mb->entries[i].active && mb->entries[i].dest_addr == beacon_src_addr) {
            mailbox_deliver(mb, &mb->entries[i], now_ms);
        }
    }
}
```

### Delivery Protocol

```
Mailbox ──MAILBOX_DELIVER──▶ Destination
Mailbox ◀──ACK───────────── Destination
```

`MAILBOX_DELIVER` wraps the original DATA packet. The destination processes the inner DATA packet as if it just arrived. Since the DATA is already E2E encrypted with the destination's key, the mailbox never decrypted it.

Delivery is **ordered by `store_time_ms`** (oldest first) and **paced** — one message per beacon interval (30s) to avoid flooding the newly-rejoined node and to respect airtime budgets. The mailbox sets a `delivery_pending` flag and drains one entry per tick.

### Deduplication

The destination already has dedup tables keyed on `packet_id`. If a message was somehow delivered by another path (e.g., the sender retried before the mailbox could deliver), the destination's existing dedup logic drops the duplicate. No changes needed.

### Delivery Receipts

When the destination ACKs a `MAILBOX_DELIVER`, the mailbox removes the entry from its buffer. It also generates a `DELIVERY_RECEIPT` routed back to the original `src_addr` so the sender knows the message was eventually delivered. The receipt's `relay_path` includes the mailbox node's address.

If the ACK is not received after `tier_max_retries()` attempts, the entry stays in the buffer for the next beacon from that destination (or until TTL expiry).

---

## 4. Security

### E2E Encryption

Messages in the mailbox are AES-256-GCM encrypted for the destination. The mailbox stores opaque ciphertext. It cannot read, modify, or forge message contents. The `dest_addr` and `src_addr` are visible in the header (as with all routed packets today).

### Store Request Authentication

`STORE_REQUEST` includes a 4-byte HMAC over the request fields, computed with the network-wide shared key (same as beacon HMAC). This prevents random non-network nodes from stuffing the mailbox.

```c
typedef struct {
    bramble_header_t header;    /* type = PKT_TYPE_STORE_REQUEST */
    uint32_t src_addr;          /* original message source */
    uint32_t orig_dest_addr;    /* intended destination */
    uint32_t orig_packet_id;    /* original packet ID for dedup */
    uint32_t ttl_s;             /* requested TTL (capped by mailbox) */
    uint16_t payload_len;
    uint8_t  payload[MAILBOX_MSG_MAX_SIZE]; /* encrypted DATA packet */
    uint8_t  auth_hmac[4];      /* HMAC of all preceding fields */
} bramble_store_request_t;
```

### Anti-Abuse

| Threat | Mitigation |
|--------|-----------|
| Flooding a destination's mailbox | Per-destination cap (`MAILBOX_MAX_PER_DEST = 8`) |
| Single source monopolizing buffer | Per-source cap (`MAILBOX_MAX_PER_SOURCE = 8`) |
| Storing huge messages | `MAILBOX_MSG_MAX_SIZE = 222` (same as DATA payload max) |
| Replay attacks | `packet_id` dedup on both store and delivery |
| Unauthenticated stores | HMAC verification; drop if invalid |
| Storage exhaustion via TTL abuse | Mailbox caps TTL to `min(requested, MAILBOX_DEFAULT_TTL_S)` |

### Future: Per-Node Store Authorization

A destination node could publish a "store policy" in its beacons or key exchange — e.g., "only accept stores from these source addresses" or "no store-and-forward for me." This is deferred to a future iteration.

---

## 5. Protocol Additions

### New Packet Types

```c
#define PKT_TYPE_STORE_REQUEST    0x0B
#define PKT_TYPE_STORE_ACK        0x0C
#define PKT_TYPE_STORE_NACK       0x0D
#define PKT_TYPE_MAILBOX_DELIVER  0x0E
```

### Packet Formats

**STORE_REQUEST** (variable length, max ~240 bytes):

```
Offset  Size  Field
  0       1   version
  1       1   type (0x0B)
  2       1   flags
  3       1   hop_limit (1 — single hop to mailbox neighbor)
  4       4   dest_addr (mailbox node address)
  8       4   packet_id
 12       4   src_addr (node requesting the store)
 16       4   orig_dest_addr (ultimate destination)
 20       4   orig_packet_id
 24       4   ttl_s
 28       2   payload_len
 30       N   payload (encrypted original DATA, N ≤ 222)
 30+N     4   auth_hmac
```

**STORE_ACK** (20 bytes):

```
Offset  Size  Field
  0       1   version
  1       1   type (0x0C)
  2       1   flags
  3       1   hop_limit (1)
  4       4   dest_addr (requester address)
  8       4   packet_id
 12       4   src_addr (mailbox node)
 16       4   orig_packet_id (echo back for correlation)
```

**STORE_NACK** (20 bytes): Same layout as STORE_ACK but type = `0x0D`. Byte 2 flags can carry a reason code:
- `0x01` — buffer full
- `0x02` — auth failed
- `0x03` — destination rejected (future)

**MAILBOX_DELIVER** (variable length, max ~238 bytes):

```
Offset  Size  Field
  0       1   version
  1       1   type (0x0E)
  2       1   flags
  3       1   hop_limit (1 — single hop, mailbox is neighbor)
  4       4   dest_addr (original destination, now online)
  8       4   packet_id
 12       4   src_addr (mailbox node address)
 16       4   orig_src_addr (original sender)
 20       4   orig_packet_id
 24       2   payload_len
 26       N   payload (encrypted original DATA)
```

The destination ACKs using the standard `PKT_TYPE_ACK` with `ack_packet_id` set to the `MAILBOX_DELIVER`'s `packet_id`.

### Modifications to Existing Flow

**RREQ/RREP:** No changes. Store-and-forward is triggered *after* route discovery fails.

**RERR handling:** After processing a RERR, the forwarding node now checks for mailbox neighbors before dropping the packet. New code path in `forwarding.c`:

```c
/* existing RERR handler */
if (rerr.broken_dest == route.next_hop) {
    routing_table_remove(&table, rerr.broken_dest);
    /* NEW: try store-and-forward before giving up */
    if (mailbox_try_store(packet, packet_len)) {
        return; /* stored successfully, don't send RERR upstream */
    }
    /* original: propagate RERR */
    send_rerr(...);
}
```

**Beacon reception:** Add `mailbox_on_beacon()` call in the beacon handler.

---

## 6. RAM Budget

### Available RAM

| | Bytes |
|---|---|
| ESP32-S3 total SRAM | ~320 KB |
| Bramble current usage | ~127 KB |
| Free | ~193 KB |
| **Mailbox budget** | **8 KB** (configurable) |

### Per-Message Cost

```
sizeof(mailbox_entry_t) = 4 + 4 + 4 + 4 + 4 + 2 + 222 + 1 = 245 bytes
```

With padding/alignment: **248 bytes** per entry.

### Capacity

| Config | Messages | RAM |
|--------|----------|-----|
| 8 KB budget | 32 messages | 7,936 bytes |
| 4 KB budget | 16 messages | 3,968 bytes |
| 16 KB budget | 64 messages | 15,872 bytes |

**Default: 32 messages / ~8 KB.** This is ~4% of free RAM. Comfortable.

### Worst Case

32 messages × 222 bytes payload = 7,104 bytes of buffered data. At LoRa SF10/125kHz (~1.2 kbps effective), draining the full buffer takes ~47 seconds of airtime — well within reason if paced over multiple beacon intervals.

---

## 7. Integration with Existing Components

### New Component: `components/mailbox/`

```
components/mailbox/
├── CMakeLists.txt
├── include/
│   └── mailbox.h
└── mailbox.c
```

**`mailbox.h`** — `mailbox_t`, `mailbox_init()`, `mailbox_store()`, `mailbox_on_beacon()`, `mailbox_tick()`, `mailbox_try_store()`

**`mailbox.c`** — buffer management, eviction, HMAC verification, delivery pacing

### Files Modified

| File | Change |
|------|--------|
| `components/packet/include/packet.h` | Add `PKT_TYPE_STORE_REQUEST/ACK/NACK/MAILBOX_DELIVER` defines, struct typedefs, serialize/deserialize declarations |
| `components/packet/packet.c` | Implement serialize/deserialize for new packet types |
| `components/routing/beacon.c` | Set `BEACON_FLAG_MAILBOX` when configured |
| `components/routing/include/beacon.h` | Add `BEACON_FLAG_MAILBOX` define |
| `components/routing/forwarding.c` | After RERR / failed RREQ, call `mailbox_try_store()` |
| `components/routing/routing.c` | Store `is_mailbox` in neighbor table on beacon rx, call `mailbox_on_beacon()` |
| `components/reliability/reliability.c` | No changes — mailbox uses separate ACK tracking |

### Dependencies

`mailbox` depends on: `packet` (serialization), `crypto` (HMAC), `routing` (neighbor table query).

---

## 8. Simulator Scenarios

### Scenario 1: Basic Store and Deliver

- 3 nodes: A (sender), M (mailbox), B (destination)
- B goes offline (stops beaconing)
- A sends message to B → RREQ fails → A stores on M
- B comes back online (resumes beaconing)
- M detects B's beacon → delivers stored message
- **Assert:** B receives the message, A receives a delivery receipt with M in the relay path

### Scenario 2: Buffer Eviction

- M has `MAILBOX_MAX_MESSAGES = 4` (reduced for test)
- A sends 6 messages to offline B
- **Assert:** oldest 2 are evicted, newest 4 are delivered when B rejoins

### Scenario 3: Per-Destination Cap

- A sends 12 messages to offline B via M (`MAX_PER_DEST = 8`)
- **Assert:** only the 8 most recent are buffered

### Scenario 4: Multiple Mailbox Nodes

- 4 nodes: A, M1 (mailbox), M2 (mailbox), B
- B offline, A has both M1 and M2 as neighbors
- A sends message → stores on preferred mailbox (better RSSI)
- **Assert:** message stored on exactly one mailbox

### Scenario 5: TTL Expiry

- A stores message on M with 60s TTL
- B does not rejoin for 90s
- **Assert:** message is evicted, not delivered

### Scenario 6: Dedup on Delivery

- A sends message to B, stores on M
- B comes online, A also retransmits directly (retry)
- M delivers stored copy
- **Assert:** B receives message exactly once (dedup by `packet_id`)

### Scenario 7: Anti-Abuse

- Rogue node R (not in network) sends STORE_REQUEST to M
- **Assert:** M rejects (HMAC verification fails, STORE_NACK with reason 0x02)

### Scenario 8: Mailbox Chain (Multi-Hop Destination)

- 5 nodes: A — C — M — D — B
- M is the only mailbox, B goes offline
- A sends to B → route via C, M, D → D gets RERR (B gone) → D forwards STORE_REQUEST back to M
- B rejoins near M (or near D, who routes from M)
- **Assert:** message eventually delivered

---

## Task Breakdown

### Phase 1: Packet Types & Serialization
- [ ] Add `PKT_TYPE_STORE_REQUEST/ACK/NACK/MAILBOX_DELIVER` to `packet.h`
- [ ] Define structs for all four packet types
- [ ] Implement serialize/deserialize in `packet.c`
- [ ] Unit tests for round-trip serialization

### Phase 2: Mailbox Component (Core)
- [ ] Create `components/mailbox/` with `mailbox.h` and `mailbox.c`
- [ ] Implement `mailbox_init()`, `mailbox_store()`, `mailbox_remove()`
- [ ] Implement eviction policy (TTL, per-dest, per-source, global)
- [ ] Implement `mailbox_tick()` for periodic expiry sweep
- [ ] Unit tests for buffer management and eviction

### Phase 3: Beacon Integration
- [ ] Add `BEACON_FLAG_MAILBOX` to `beacon.h`
- [ ] Set flag in `beacon_build()` when `CONFIG_BRAMBLE_MAILBOX_ENABLED`
- [ ] Parse flag in beacon rx, store `is_mailbox` in neighbor table
- [ ] Implement `mailbox_on_beacon()` — scan buffer for deliverable messages

### Phase 4: Store Flow
- [ ] In `forwarding.c`: after RREQ failure / RERR, try `mailbox_try_store()`
- [ ] Implement STORE_REQUEST send logic (select best mailbox neighbor)
- [ ] Implement STORE_REQUEST receive + HMAC verify + STORE_ACK/NACK response
- [ ] Add pending_ack entry for STORE_REQUEST (retry if no ACK)

### Phase 5: Delivery Flow
- [ ] Implement `mailbox_deliver()` — send MAILBOX_DELIVER, pace one per interval
- [ ] Handle ACK for MAILBOX_DELIVER — remove from buffer
- [ ] Generate DELIVERY_RECEIPT back to original source on successful delivery
- [ ] Handle delivery retry on ACK timeout

### Phase 6: Simulator Integration
- [ ] Add mailbox state to `sim_node_t`
- [ ] Include `mailbox.c` in simulator build (`all.c`)
- [ ] Wire `mailbox_on_beacon()` into sim beacon handler
- [ ] Implement scenarios 1–8 above
- [ ] Add metrics: `messages_stored`, `messages_delivered_from_mailbox`, `messages_expired`, `store_rejects`

### Phase 7: Tuning & Docs
- [ ] Test RAM usage on real ESP32-S3 hardware
- [ ] Tune defaults (TTL, buffer size, pacing interval)
- [ ] Document in `docs/` — user-facing config guide
- [ ] Add mailbox status to BLE/display UI
