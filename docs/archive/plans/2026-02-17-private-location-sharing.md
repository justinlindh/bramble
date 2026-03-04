# Private Location Sharing

> ✅ **SIMULATOR IMPLEMENTATION COMPLETE** (2026-02-17) — Checkboxes below not updated but all tasks were implemented in the simulator branch.

> **Status:** Draft — design phase  
> **Date:** 2026-02-17  
> **Component:** `components/location/`  
> **Dependencies:** `crypto`, `routing`, `reliability`, `packet`, `identity`, `ble`

---

## Motivation

Meshtastic broadcasts GPS positions to every node on the channel — a massive privacy leak and airtime waste. A 20-node mesh with 15-minute position intervals generates 20 × 4/hr = 80 position broadcasts per hour, each flooded to all nodes. MeshCore similarly embeds position in node adverts visible to the entire mesh.

Bramble's core design principle is **privacy by default**. Location is the most sensitive data a mesh node can share. It should never be broadcast, never visible to relay nodes, and only shared with explicitly approved contacts.

This plan defines a selective, encrypted, per-contact location sharing system built on Bramble's existing E2E encrypted DM infrastructure.

---

## 1. Selective Sharing Model

### Design

Location sharing uses an **explicit allowlist** per contact. No position data is ever broadcast, included in beacons, or sent via channel messages.

```
Contact: alice
  location_share: FULL
  update_interval: 300s
  distance_trigger: 100m

Contact: bob
  location_share: COARSE
  update_interval: 600s
  distance_trigger: 500m

Contact: carol
  location_share: OFF
```

**Sharing states per contact:**
- `OFF` — no position shared (default for all contacts)
- `FULL` — lat, lon, alt, speed, heading, accuracy
- `COARSE` — Maidenhead grid square (~1.1km resolution, 4-character)
- `PRESENCE` — online/offline indicator only, no coordinates

Sharing is **unidirectional** — Alice sharing with Bob does not imply Bob shares with Alice. Each direction is independently configured.

### Airtime Comparison

| Scenario | Meshtastic (20 nodes, 15 min broadcast) | Bramble (5 approved contacts, 5 min DM) |
|---|---|---|
| Packets/hour originated | 80 (broadcast) | 60 (unicast DMs) |
| Packets/hour on-air (flooding, avg 4 hops) | ~320 | ~180 (routed, avg 3 hops) |
| Privacy | Everyone sees everyone | Only approved contacts |
| Airtime per position (broadcast flood) | ~320 × 40ms = 12.8s | ~180 × 35ms = 6.3s |

At 10 contacts the airtime cost approaches Meshtastic's flood — but each packet is routed (not flooded) and encrypted E2E, so relay nodes learn nothing. Distance-based triggers further reduce traffic when nodes are stationary.

---

## 2. Update Frequency and Triggers

### Configurable Parameters

| Parameter | Default | Range | Notes |
|---|---|---|---|
| `update_interval` | 300s (5 min) | 60s–3600s | Per-contact override possible |
| `distance_trigger` | 100m | 10m–5000m | Send update if moved > threshold since last sent |
| `speed_trigger` | enabled | on/off | Increase update rate when moving (>2 km/h) |
| `stationary_backoff` | 4× | 1×–16× | Multiply interval when stationary (no distance trigger) |
| `manual_share` | always available | — | User pushes position on demand |

### Update Logic (Pseudocode)

```c
void location_update_tick(void) {
    position_t pos = gps_get_position();  // or BLE companion
    
    for (contact_t *c = sharing_list; c; c = c->next) {
        if (c->share_tier == SHARE_OFF) continue;
        
        uint32_t interval = c->update_interval;
        if (is_stationary(pos, c->last_sent_pos, c->distance_trigger)) {
            interval *= c->stationary_backoff;
        }
        
        bool time_due = (now() - c->last_sent_time) >= interval;
        bool dist_due = haversine(pos, c->last_sent_pos) >= c->distance_trigger;
        bool manual   = c->manual_pending;
        
        if (time_due || dist_due || manual) {
            location_pkt_t pkt = build_position_packet(pos, c->share_tier);
            dm_send_encrypted(c->identity, PKT_LOCATION_UPDATE, &pkt, sizeof(pkt));
            c->last_sent_pos  = pos;
            c->last_sent_time = now();
            c->manual_pending = false;
        }
    }
}
```

### Airtime Budget Integration

Location updates use the existing airtime token bucket. Position packets are **low priority** — they yield to DMs, reliability ACKs, and route discovery. If the airtime budget is exhausted, position updates are deferred (not dropped — queued for next available slot).

---

## 3. Position Packet Format

### `LOCATION_UPDATE` (17 bytes payload)

```
Offset  Size  Field         Encoding
0       4     latitude      int32_t, degrees × 1e7 (±90° → ±900,000,000)
4       4     longitude     int32_t, degrees × 1e7 (±180° → ±1,800,000,000)
8       2     altitude      int16_t, meters (-32768 to +32767)
10      1     accuracy      uint8_t, meters (0-255, clamped)
11      1     speed         uint8_t, km/h (0-255, clamped)
12      1     heading       uint8_t, degrees/1.41 (0-255 → 0-359°)
13      4     timestamp     uint32_t, mesh epoch seconds
```

Total: **17 bytes** payload.

With Bramble packet overhead:
- Header: 8 bytes (type, flags, src_hash, dst_hash, hop_count, seq)
- Crypto: 16 bytes (AES-256-GCM tag) + 8 bytes (nonce)
- **Total on-air: 49 bytes** — fits comfortably in a single LoRa frame at any SF

### `LOCATION_COARSE` (5 bytes payload)

```
Offset  Size  Field         Encoding
0       4     grid_square   4 ASCII chars (Maidenhead, e.g. "CM87")
4       1     flags         uint8_t (bit 0: online, bits 1-7: reserved)
```

### `LOCATION_PRESENCE` (1 byte payload)

```
Offset  Size  Field         Encoding
0       1     flags         uint8_t (bit 0: online, bit 1: has_gps, bits 2-7: reserved)
```

### Packet Types

| Type ID | Name | Direction | Payload |
|---|---|---|---|
| `0x20` | `LOCATION_UPDATE` | push | 17 bytes (full position) |
| `0x21` | `LOCATION_COARSE` | push | 5 bytes (grid square) |
| `0x22` | `LOCATION_PRESENCE` | push | 1 byte (online/offline) |
| `0x23` | `LOCATION_REQUEST` | pull | 1 byte (requested tier) |
| `0x24` | `LOCATION_RESPONSE` | pull reply | same as 0x20/0x21/0x22 |
| `0x25` | `GEOFENCE_ALERT` | push | 9 bytes (see §6) |

All location packet types are sent as E2E encrypted DMs using existing `dm_send_encrypted()`.

---

## 4. Privacy Tiers

### Tier Details

| Tier | Data Shared | Resolution | Use Case |
|---|---|---|---|
| **FULL** | lat, lon, alt, speed, heading, accuracy | ~1m (GPS) | Trusted contacts, SAR, hiking partners |
| **COARSE** | Maidenhead grid square (4-char) | ~1.1 km | Casual contacts, "which area are you in" |
| **PRESENCE** | online/offline flag | None | Acquaintances, "are you on the mesh" |
| **OFF** | Nothing | N/A | Default for all contacts |

### Per-Contact Configuration

Each contact in the identity store gets a `location_config` struct:

```c
typedef struct {
    uint8_t  share_tier;        // SHARE_OFF, SHARE_FULL, SHARE_COARSE, SHARE_PRESENCE
    uint16_t update_interval;   // seconds
    uint16_t distance_trigger;  // meters
    uint8_t  stationary_backoff;// multiplier
    uint8_t  auto_approve_req;  // auto-respond to LOCATION_REQUEST?
    uint8_t  geofence_count;    // number of active geofences for this contact
} location_config_t;
```

Stored in NVS alongside the contact's identity/key material.

---

## 5. Location Request/Response

A contact can request your current position without you actively sharing.

### Flow

```
Bob → Alice:  LOCATION_REQUEST { tier: FULL }
              (encrypted DM)

Alice's node: check auto_approve for Bob
  if auto_approve: immediately reply
  if manual:       show prompt on display / send BLE notification to companion app

Alice → Bob:  LOCATION_RESPONSE { <position at requested or lower tier> }
              (encrypted DM, RELIABLE tier for guaranteed delivery)
```

### Request Packet (1 byte)

```
Offset  Size  Field           Encoding
0       1     requested_tier  uint8_t (FULL=0x01, COARSE=0x02, PRESENCE=0x03)
```

### Behavior Rules

- Response tier is `min(requested_tier, configured_share_tier_for_contact)`
- If `share_tier == OFF` for that contact, the request is silently dropped (no response — avoids leaking that the node is online)
- Requests are rate-limited: max 1 per 60 seconds per contact (prevents position tracking abuse)
- Request/response uses RELIABLE delivery tier (ACK + retry)

---

## 6. Geofence Alerts

Optional feature for SAR, group hiking, and safety scenarios.

### Geofence Definition

```c
typedef struct {
    int32_t  center_lat;    // degrees × 1e7
    int32_t  center_lon;    // degrees × 1e7
    uint16_t radius_m;      // meters (max 65535m ≈ 65km)
    uint8_t  flags;         // bit 0: alert on enter, bit 1: alert on exit
    uint8_t  geofence_id;   // 0-255, local identifier
} geofence_t;
```

Geofences are configured locally and evaluated locally. When a trigger fires, a `GEOFENCE_ALERT` is sent to the associated contact(s).

### `GEOFENCE_ALERT` Packet (9 bytes)

```
Offset  Size  Field           Encoding
0       1     geofence_id     uint8_t
1       1     event           uint8_t (0x01=enter, 0x02=exit)
2       4     latitude        int32_t (current position)
6       2     altitude        int16_t
8       1     accuracy        uint8_t
```

The contact receiving the alert must already be in the sharing allowlist. Geofence alerts use CRITICAL reliability tier.

### Limits

- Max 8 geofences per contact, 32 total per node (RAM constraint on ESP32)
- Geofence evaluation runs on every GPS fix (~1 Hz) — trivial CPU cost (haversine distance check)
- Hysteresis: 20m buffer to prevent rapid enter/exit oscillation at boundary

---

## 7. Last-Known Position Cache

### Local Storage

Each contact with location sharing (in either direction) gets a cached position:

```c
typedef struct {
    int32_t  latitude;
    int32_t  longitude;
    int16_t  altitude;
    uint8_t  accuracy;
    uint8_t  speed;
    uint8_t  heading;
    uint32_t timestamp;       // mesh epoch
    uint32_t received_at;     // local clock when received
    uint8_t  tier;            // tier of this data
    bool     stale;           // true if older than TTL
} cached_position_t;
```

Stored in NVS. Survives reboots.

### TTL and Expiry

- Default TTL: 1 hour (configurable 5 min – 24 hours)
- After TTL: position marked `stale` (still displayed with visual indicator, not deleted)
- After 24 hours: position purged from cache
- UI shows: timestamp, staleness indicator, and time-since-update

### Store-and-Forward Interaction

When a node goes offline and comes back, the store-and-forward system may deliver queued position updates. The cache accepts these and updates if the timestamp is newer than cached. Out-of-order packets are handled by timestamp comparison — newer always wins.

---

## 8. Security

### Threat Model

| Threat | Mitigation |
|---|---|
| Relay node learns user positions | All position data E2E encrypted. Relay sees only opaque ciphertext + src/dst hashes (which are rotated). |
| Position in beacons/adverts | Never. Beacons contain zero location data (unlike Meshtastic/MeshCore). |
| Replay attack (old position re-sent) | Nonce/counter in DM crypto. Duplicate positions rejected by seq number + dedup. |
| Traffic analysis (timing reveals position updates) | Position packets are same size as text DMs. Random jitter (0-30s) added to update schedule. |
| Compromised contact shares your position | Inherent risk in any sharing system. Mitigated by per-contact tier control and ability to revoke instantly. |
| Physical device capture | Position cache encrypted at rest (existing NVS encryption). Cache auto-purges after TTL. |

### Jitter

All scheduled position updates add uniform random jitter of 0–30 seconds to prevent timing-based traffic analysis. A relay node seeing periodic encrypted packets from a source cannot distinguish position updates from text messages.

### Revocation

Setting a contact's tier to `OFF` immediately:
1. Stops all future position updates to that contact
2. Sends no notification (silent — avoids revealing the action)
3. Local cache of *their* position is retained (your data, your choice)

---

## 9. Integration

### New Component: `components/location/`

```
components/location/
├── CMakeLists.txt
├── include/
│   └── location.h          // Public API
├── location_manager.c      // Update loop, sharing logic, cache
├── location_packets.c      // Packet encode/decode
├── location_geofence.c     // Geofence evaluation
├── location_gps.c          // GPS NMEA parsing (UART)
├── location_ble.c          // Position from BLE companion app
└── location_config.c       // NVS storage for per-contact config
```

### Dependencies

```
location → crypto      (E2E encryption of position packets)
location → routing     (dm_send_encrypted for unicast delivery)
location → reliability (RELIABLE/CRITICAL tiers for requests/geofence)
location → packet      (packet type registration, encode/decode)
location → identity    (contact list, key lookup)
location → ble         (companion app position injection)
location → airtime     (budget check before transmit)
```

### Public API

```c
// Initialize location subsystem
void location_init(void);

// Set sharing config for a contact
void location_set_share(identity_t *contact, location_config_t *config);

// Manual position share to a contact
void location_share_now(identity_t *contact);

// Request position from a contact
void location_request(identity_t *contact, uint8_t tier);

// Get cached position for a contact (returns false if none/expired)
bool location_get_cached(identity_t *contact, cached_position_t *out);

// Add/remove geofence
bool location_geofence_add(identity_t *contact, geofence_t *fence);
void location_geofence_remove(identity_t *contact, uint8_t geofence_id);

// Called by packet handler when location packet received
void location_handle_packet(packet_t *pkt);
```

### GPS Source Abstraction

```c
typedef struct {
    bool (*get_position)(position_t *out);
    bool (*is_available)(void);
    const char *name;  // "gps_uart", "ble_companion", "manual"
} location_source_t;
```

Priority: GPS UART > BLE companion > manual entry. First available source wins.

---

## 10. Simulator Scenarios

### Scenario 1: Basic Selective Sharing (3 nodes)

```
Topology: A ←→ B ←→ C (linear)

Config:
  A shares FULL with B (interval=120s)
  A shares nothing with C
  
Verify:
  - B receives position updates from A every ~120s
  - C never receives any position data from A
  - Relay B cannot read A's position packets destined elsewhere
  - Packet count matches expected: ~30 packets/hour A→B
```

### Scenario 2: Privacy Tiers (4 nodes)

```
Topology: A ←→ R1 ←→ R2 ←→ B (relayed)

Config:
  A shares FULL with B
  A shares COARSE with R1
  A shares PRESENCE with R2

Verify:
  - B receives 17-byte LOCATION_UPDATE
  - R1 receives 5-byte LOCATION_COARSE
  - R2 receives 1-byte LOCATION_PRESENCE
  - Packet sizes on wire match expected (verify relay doesn't see plaintext)
```

### Scenario 3: Distance-Based Trigger (2 nodes, simulated movement)

```
Topology: A ←→ B (direct)

Config:
  A shares FULL with B, interval=300s, distance_trigger=100m
  Simulate A moving: stationary 10 min, then 500m movement, then stationary

Verify:
  - During stationary: updates at 300s × backoff interval
  - On movement: immediate update when 100m threshold crossed
  - After stopping: return to backoff interval
  - Total packet count < naive 300s interval
```

### Scenario 4: Location Request/Response (3 nodes)

```
Topology: A ←→ R ←→ B

Config:
  A has auto_approve=true for B
  B sends LOCATION_REQUEST to A

Verify:
  - B receives LOCATION_RESPONSE within routing RTT + processing
  - R relays but cannot read position
  - Request rate limiting: second request within 60s is silently dropped by A
```

### Scenario 5: Geofence Alert (2 nodes, simulated movement)

```
Topology: A ←→ B (direct)

Config:
  A configures geofence: center=(37.7749, -122.4194), radius=500m, alert_on=enter|exit
  A shares with B, geofence tied to B
  Simulate A moving from 1km away → into geofence → back out

Verify:
  - GEOFENCE_ALERT (enter) sent when A crosses 500m boundary inward
  - GEOFENCE_ALERT (exit) sent when A crosses 520m boundary outward (hysteresis)
  - Alerts use CRITICAL reliability
  - No spurious alerts at boundary (hysteresis working)
```

### Scenario 6: Airtime Stress (10 nodes, fully connected)

```
Topology: 10 nodes, all within range

Config:
  Each node shares FULL with 5 others, interval=120s
  Background DM traffic: 2 messages/min mesh-wide

Verify:
  - Position updates don't starve DM traffic
  - Airtime budget respected: position deferred when budget low
  - Total airtime < 10% duty cycle per node
  - Compare with equivalent Meshtastic scenario (10 nodes broadcasting)
```

---

## Task Breakdown

### Phase 1: Core Infrastructure

- [ ] **T1.1** Create `components/location/` directory structure and CMakeLists.txt
- [ ] **T1.2** Define packet types (0x20–0x25) in packet registry
- [ ] **T1.3** Implement `location_packets.c` — encode/decode for all 6 packet types
- [ ] **T1.4** Implement `location_config.c` — NVS storage for per-contact location config
- [ ] **T1.5** Unit tests for packet encode/decode (round-trip fuzz)

### Phase 2: GPS Input

- [ ] **T2.1** Implement `location_gps.c` — UART NMEA parser for GPS module (T-Beam)
- [ ] **T2.2** Implement `location_ble.c` — position injection from BLE companion app
- [ ] **T2.3** Implement location source abstraction (priority fallback)
- [ ] **T2.4** Test GPS parsing with simulated NMEA sentences

### Phase 3: Sharing Engine

- [ ] **T3.1** Implement `location_manager.c` — update loop with interval/distance/manual triggers
- [ ] **T3.2** Integrate with airtime budget (low-priority queue)
- [ ] **T3.3** Add random jitter (0–30s) to scheduled updates
- [ ] **T3.4** Implement stationary backoff logic
- [ ] **T3.5** Implement location request/response flow
- [ ] **T3.6** Implement rate limiting on incoming requests

### Phase 4: Cache and Storage

- [ ] **T4.1** Implement `cached_position_t` storage in NVS
- [ ] **T4.2** TTL expiry and auto-purge logic
- [ ] **T4.3** Store-and-forward integration (accept delayed position updates)

### Phase 5: Geofencing

- [ ] **T5.1** Implement `location_geofence.c` — haversine evaluation, hysteresis
- [ ] **T5.2** Geofence CRUD (add/remove/list)
- [ ] **T5.3** GEOFENCE_ALERT packet generation on trigger

### Phase 6: UI and Companion

- [ ] **T6.1** Display integration — show contact positions on screen
- [ ] **T6.2** BLE companion protocol — share config, position display, geofence setup
- [ ] **T6.3** Web app — map view of shared contacts (if web app exists)

### Phase 7: Simulation

- [ ] **T7.1** Add simulated GPS source to simulator
- [ ] **T7.2** Implement movement patterns (stationary, linear, random walk)
- [ ] **T7.3** Build scenarios 1–6 as automated test cases
- [ ] **T7.4** Airtime comparison benchmark: Bramble location vs. Meshtastic position broadcast

### Phase 8: Testing

- [ ] **T8.1** Unit tests: packet encode/decode, geofence math, config storage
- [ ] **T8.2** Integration tests: sharing flow E2E with simulated nodes
- [ ] **T8.3** Security audit: verify no position leakage in beacons, relay logs, or unencrypted fields
- [ ] **T8.4** Airtime budget compliance under load

---

## Open Questions

1. **Group location sharing?** — Should there be a way to share position with a channel (encrypted to channel key)? Trades privacy for convenience. Leaning no — use per-contact sharing even in group scenarios.
2. **Position aggregation on companion app?** — Companion app could show all shared contacts on a map. Protocol-level this is just rendering cached positions. No new packets needed.
3. **Emergency broadcast position?** — SOS mode that broadcasts position to all contacts regardless of sharing config? Useful for SAR. Could be a separate `LOCATION_SOS` packet type with CRITICAL reliability.
4. **Altitude encoding?** — int16_t caps at 32,767m. Sufficient for terrestrial use but worth noting. Dead Sea (-430m) to Everest (8,849m) covered.
