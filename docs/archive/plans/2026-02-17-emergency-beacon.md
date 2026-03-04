# Feature Plan: Panic/Emergency Beacon Mode

> ✅ **SIMULATOR IMPLEMENTATION COMPLETE** (2026-02-17) — Checkboxes below not updated but all tasks were implemented in the simulator branch.

**Date:** 2026-02-17
**Status:** Draft
**Component:** `components/emergency/`

## Motivation

No existing LoRa mesh protocol (Meshtastic, MeshCore) treats emergency signaling as a first-class protocol feature. They rely on ad-hoc text messages or out-of-band coordination. Bramble's existing infrastructure — 3-tier reliability, airtime budgeting with priority queuing, delivery receipts with relay paths — provides a natural foundation for a dedicated emergency mode that is harder to miss, impossible to deprioritize, and authenticated against spoofing.

## Design Overview

Emergency mode is a special node state activated by physical button or BLE command. It broadcasts a new `PKT_TYPE_EMERGENCY` packet at Critical reliability tier on a reserved emergency channel that **all nodes monitor regardless of their configured channel**. Emergency packets carry a header flag that grants them protocol-level privileges: airtime budget exemption, extended hop limits, and relay obligation.

---

## 1. Activation

### Physical Button
- Long-press USER button (GPIO 0 on Heltec V3, GPIO 38 on T-Beam) for **3 seconds**.
- Confirmation feedback:
  - LED: 3 rapid red flashes (or white if single-color LED)
  - Buzzer: 3 short beeps (if buzzer connected via GPIO, e.g., GPIO 13)
  - OLED: `!! EMERGENCY ACTIVE !!` banner
- Debounce: ignore presses < 3s. Require button held continuously (not cumulative).

### BLE Companion App
- New BLE characteristic `EMERGENCY_ACTIVATE` (`0xBE01`) under Bramble service.
- Write `0x01` to activate, `0x00` to cancel.
- App should require confirmation dialog ("Are you sure?") — but that's app-side, not protocol.

### State Machine
```
IDLE --(long-press 3s | BLE 0x01)--> EMERGENCY_ACTIVE
EMERGENCY_ACTIVE --(long-press 3s | BLE 0x00 | timeout 24h)--> EMERGENCY_CANCELLING
EMERGENCY_CANCELLING --(cancel ACK'd or timeout 5min)--> IDLE
```

Auto-timeout at **24 hours** prevents forgotten activations from polluting the mesh indefinitely.

---

## 2. Emergency Beacon Behavior

### Beacon Interval
- First beacon: **immediately** on activation.
- Subsequent beacons: every **30 seconds** for the first 5 minutes, then every **2 minutes** thereafter.
- Rationale: front-load transmissions when situation is most urgent and rescuers are most likely to be mobilizing. Taper to conserve battery for extended emergencies.

### Emergency Channel
- Reserved channel index `0xFF` — hardcoded, not user-configurable.
- All nodes join this channel on boot in **listen-only** mode (no periodic beacons, no normal traffic).
- Emergency channel uses a well-known PSK (or plaintext with signing) so any Bramble node can decode.
- LoRa parameters: **SF12 / BW125 / CR4/8** — maximum range, lowest data rate. Emergency packets are small enough that the long airtime is acceptable.

### Beacon Payload

```c
typedef struct __attribute__((packed)) {
    uint8_t  subtype;        // 0x01 = DISTRESS, 0x02 = CANCEL
    uint8_t  flags;          // bit 0: has_gps, bit 1: has_text, bit 2: gps_is_stale
    int32_t  latitude;       // degrees * 1e7 (0 if no GPS)
    int32_t  longitude;      // degrees * 1e7 (0 if no GPS)
    int16_t  altitude_m;     // meters, signed (INT16_MIN if unknown)
    uint32_t timestamp;      // Unix epoch (seconds)
    uint16_t gps_age_s;      // seconds since GPS fix (0 = live, 0xFFFF = unknown)
    uint8_t  battery_pct;    // 0-100, 0xFF = unknown
    uint8_t  text_len;       // 0-32
    uint8_t  text[32];       // UTF-8, not null-terminated, padded with 0x00
} bramble_emergency_payload_t;  // 49 bytes max
```

Total on-air packet with Bramble header (~12 bytes) + payload (49 bytes) = **~61 bytes**. At SF12/BW125 this is ~1.8s airtime — acceptable for emergency use.

---

## 3. Relay Priority

### Header Flag

Add `BRAMBLE_FLAG_EMERGENCY` to `bramble_header_t.flags`:

```c
// bramble_header.h — existing flags
#define BRAMBLE_FLAG_WANT_ACK    (1 << 0)
#define BRAMBLE_FLAG_IS_RELAY    (1 << 1)
#define BRAMBLE_FLAG_CRITICAL    (1 << 2)
// new
#define BRAMBLE_FLAG_EMERGENCY   (1 << 3)  // bit 3
```

### Relay Behavior

When a node receives a packet with `BRAMBLE_FLAG_EMERGENCY`:

| Behavior | Normal Packet | Emergency Packet |
|---|---|---|
| Airtime budget check | Yes | **Bypassed** |
| TX power | Configured level | **Max available** (+22 dBm SX1262) |
| Hop limit | As set by sender (default 3) | **Extended to 7** (or `max(original, 7)`) |
| Relay obligation | Best-effort | **Must relay** (one rebroadcast minimum) |
| Queue priority | By tier | **Head of queue** (preempts all) |
| Duplicate window | 60s | **Extended to 300s** (5 min, since beacon repeats) |

### Relay Protocol
1. On receipt of emergency packet not yet in dedup cache:
   - Store in dedup cache (keyed by `source_addr + sequence`).
   - Relay with randomized delay **100-500ms** (collision avoidance, shorter than normal 200-2000ms).
   - Decrement `hop_count`, increment `relay_count`.
2. If already in dedup cache: do not relay, but still process locally (update tracking).

---

## 4. Notification Propagation

### Local Storage
Each node maintains an **active emergency table** (max 8 entries, LRU eviction):

```c
typedef struct {
    uint32_t source_addr;
    uint32_t first_seen;       // epoch
    uint32_t last_seen;        // epoch
    uint8_t  beacon_count;     // times received
    int32_t  last_lat;
    int32_t  last_lon;
    uint8_t  last_battery;
    uint8_t  last_text[32];
    bool     cancelled;
} emergency_tracker_entry_t;
```

### User Notification
On first receipt of a new emergency (source_addr not in table):
- **OLED:** Full-screen alert with source node name/address, distance (if own GPS available), bearing.
- **LED:** Continuous slow red pulse until user acknowledges.
- **Buzzer:** Repeating alert tone (3 long beeps, 5s pause, repeat × 3).
- **BLE:** Push notification to companion app via `EMERGENCY_NOTIFY` characteristic (`0xBE02`). Payload mirrors the emergency beacon payload.

On subsequent beacons from same source:
- Update position/battery silently.
- Flash LED briefly (single pulse).
- App gets updated data via BLE notify.

### Companion App Display
- Map view showing emergency node's position relative to user.
- Distance and bearing.
- Time since first alert.
- Battery level of distressed node.
- "Navigate to" button (opens system maps app with coordinates).

---

## 5. Cancellation

### Cancel Packet
- Same `PKT_TYPE_EMERGENCY` with `subtype = 0x02` (CANCEL).
- **Must be authenticated**: cancel payload includes an Ed25519 signature over `(source_addr || sequence || subtype || timestamp)` using the node's identity key.
- Only the originating node can cancel its own emergency.

```c
typedef struct __attribute__((packed)) {
    uint8_t  subtype;        // 0x02 = CANCEL
    uint8_t  reserved;
    uint32_t timestamp;
    uint8_t  signature[64];  // Ed25519 over (source_addr || seq || subtype || timestamp)
} bramble_emergency_cancel_t;  // 70 bytes
```

### Cancel Propagation
- Relayed with same `BRAMBLE_FLAG_EMERGENCY` privileges.
- Receiving nodes mark the emergency as `cancelled` in their tracker table.
- OLED shows "Emergency cancelled" for source node.
- Entry removed from active table after 10 minutes.

### Why Not Sign the Distress Beacon Too?
- Distress beacons are high-frequency (every 30s–2min). Signing every one adds 64 bytes and CPU cost.
- False distress is less dangerous than false cancellation. A spoofed distress wastes attention; a spoofed cancel could get someone killed.
- **Decision:** Sign cancels only. Distress beacons are implicitly authenticated by being tied to a known source_addr (which is derived from the node's public key in Bramble's identity system).

---

## 6. Anti-Abuse

### Rate Limiting
- **Per-node cooldown:** After cancellation, a node cannot re-activate emergency mode for **15 minutes**. Prevents button-mashing / accidental re-triggers.
- **No per-hour hard limit.** Rationale: if someone is genuinely in repeated danger, a rate limit could be fatal. The 15-minute post-cancel cooldown is sufficient to prevent accidental abuse.
- Nodes that relay do **not** rate-limit relaying of others' emergencies — relay obligation is unconditional.

### Abuse Mitigation
- In small mesh communities (Bramble's target), social accountability is the primary mechanism.
- Companion app can **mute** a specific source_addr's emergency alerts (local only, does not affect relaying).
- Future consideration: reputation scoring. Out of scope for v1.

---

## 7. GPS Integration

### GPS Available (T-Beam, external UART GPS)
- Include live fix in every beacon: `has_gps = 1`, `gps_is_stale = 0`, `gps_age_s = 0`.
- If GPS has fix but it's older than 30s (moving indoors?): `gps_is_stale = 1`, `gps_age_s` = actual age.
- Force GPS module to stay awake in emergency mode (override any power-saving GPS duty cycle).

### No GPS (Heltec V3 without external module)
- Companion app can push last-known position via BLE characteristic `NODE_POSITION` (`0xBE03`).
- Beacon includes this position with `has_gps = 1`, `gps_is_stale = 1`, `gps_age_s` = seconds since app provided it.
- If no position available at all: `has_gps = 0`, lat/lon = 0. Receivers show "Unknown location."

### Position Privacy
- Emergency mode is an explicit opt-in to share location. No privacy concern — user is asking for help.

---

## 8. Power Management

### Emergency Mode Power Profile
| Parameter | Normal Mode | Emergency Mode |
|---|---|---|
| CPU | Per sleep config | **Active, no sleep** |
| Radio TX power | Configured (typ. +10 to +17 dBm) | **+22 dBm (max SX1262)** |
| Radio RX | Duty-cycled per config | **Continuous RX** (listen for cancel ACKs, other emergencies) |
| GPS | Duty-cycled or off | **Continuous** (if available) |
| OLED | Per timeout | **On** (shows emergency status) |
| Airtime budget | Enforced | **Suspended** |
| Deep sleep | Enabled | **Disabled** |

### Battery Estimation
- Display estimated remaining time in emergency mode on OLED.
- Rough estimate: Heltec V3 (1100mAh) at max TX every 2 min ≈ **6-8 hours**. T-Beam (6000mAh) ≈ **30+ hours**.
- Include `battery_pct` in every beacon so rescuers can assess urgency.

### Low Battery Behavior
- At **10% battery**: OLED warning, increase beacon interval to **5 minutes**.
- At **5% battery**: single final beacon with `text = "LOW BATT"`, then deep sleep with GPIO wake (button press restarts emergency mode if power recovers via solar).

---

## 9. Protocol Additions

### New Packet Type

```c
// bramble_packet_types.h
#define PKT_TYPE_EMERGENCY  0x08  // Emergency beacon / cancel
```

### Header Changes

Existing `bramble_header_t` (assumed structure):

```c
typedef struct __attribute__((packed)) {
    uint8_t  version    : 4;
    uint8_t  hop_limit  : 4;
    uint8_t  hop_count;
    uint8_t  flags;          // BRAMBLE_FLAG_EMERGENCY is bit 3
    uint8_t  pkt_type;
    uint32_t dest_addr;      // 0xFFFFFFFF for broadcast
    uint32_t source_addr;
    uint16_t sequence;
    uint8_t  payload_len;
} bramble_header_t;  // 14 bytes
```

Emergency packets set:
- `flags |= BRAMBLE_FLAG_EMERGENCY | BRAMBLE_FLAG_CRITICAL`
- `pkt_type = PKT_TYPE_EMERGENCY`
- `dest_addr = 0xFFFFFFFF` (broadcast)
- `hop_limit = 7`

### Wire Format Summary

```
[bramble_header_t: 14 bytes][emergency_payload: 49 bytes]
Total: 63 bytes (DISTRESS)

[bramble_header_t: 14 bytes][emergency_cancel: 70 bytes]
Total: 84 bytes (CANCEL)
```

---

## 10. Integration

### Component Structure

```
components/emergency/
├── include/
│   └── emergency.h          // Public API
├── src/
│   ├── emergency.c          // State machine, activation logic
│   ├── emergency_beacon.c   // Beacon construction and scheduling
│   ├── emergency_relay.c    // Relay handling and priority logic
│   └── emergency_tracker.c  // Active emergency table
├── test/
│   └── test_emergency.c     // Unity tests
└── CMakeLists.txt
```

### Interactions with Existing Components

| Component | Interaction |
|---|---|
| `components/airtime/` | Emergency relay bypasses `airtime_can_transmit()`. New `airtime_emergency_override(bool)` API. |
| `components/reliability/` | Emergency packets always use Critical tier internally but have their own beacon scheduler (not the standard retry logic). |
| `components/radio/` | `radio_set_tx_power(RADIO_TX_MAX)` when in emergency mode or relaying emergency packet. |
| `components/mesh/` | `mesh_on_rx()` checks `BRAMBLE_FLAG_EMERGENCY` before dedup/relay logic. Emergency packets take a dedicated code path. |
| `components/ble/` | New characteristics `0xBE01` (activate), `0xBE02` (notify), `0xBE03` (position inject). |
| `components/ui/` | Emergency state triggers full-screen OLED alert, LED patterns. New UI state `UI_STATE_EMERGENCY`. |
| `components/gps/` | `gps_force_continuous(bool)` to override duty cycling. |
| `components/power/` | `power_emergency_mode(bool)` disables sleep, reports battery estimate. |

---

## 11. Simulator Scenarios

### Scenario A: Basic Activation and Propagation
```
[Node A] --(activate)--> broadcasts DISTRESS
[Node B] receives, relays, alerts user
[Node C] receives from B, relays, alerts user
Verify: all nodes show alert within ~5s. Beacon repeats on schedule.
```

### Scenario B: Multi-Hop with Airtime Exhaustion
```
[Node B] airtime budget = 0% remaining
[Node A] activates emergency
Verify: Node B relays despite exhausted budget.
```

### Scenario C: Cancellation Authentication
```
[Node A] activates emergency, mesh propagates
[Node A] sends CANCEL (signed)
Verify: all nodes mark cancelled.
[Node X] sends spoofed CANCEL for Node A
Verify: all nodes reject (signature invalid).
```

### Scenario D: GPS Stale Position
```
[Node A] has no GPS. Companion app pushed position 10 min ago.
[Node A] activates emergency.
Verify: beacon contains position with gps_is_stale=1, gps_age_s=600.
```

### Scenario E: Low Battery Degradation
```
[Node A] battery at 12%, activates emergency.
Verify: normal beacon interval.
[Node A] battery drops to 9%.
Verify: beacon interval increases to 5 min, OLED warning.
[Node A] battery drops to 4%.
Verify: final beacon sent with "LOW BATT", node enters deep sleep.
```

### Scenario F: Hop Limit Extension
```
Linear topology: A -- B -- C -- D -- E -- F -- G -- H
[Node A] activates emergency (hop_limit=7).
Verify: beacon reaches Node H.
Normal packet with hop_limit=3 would die at Node D.
```

### Scenario G: Duplicate Suppression with Repeated Beacons
```
[Node A] sends beacon every 30s.
[Node B] receives and relays first beacon.
[Node B] receives second beacon (new sequence number).
Verify: Node B relays second beacon too (different sequence).
Verify: Node B does NOT relay duplicate of first beacon (same sequence, within 300s window).
```

---

## Task Breakdown

### Phase 1: Protocol & Core (Est. 3-4 days)
- [ ] Define `PKT_TYPE_EMERGENCY` and `BRAMBLE_FLAG_EMERGENCY` in header files
- [ ] Implement `bramble_emergency_payload_t` and `bramble_emergency_cancel_t` serialization
- [ ] Add emergency flag check to `mesh_on_rx()` relay path
- [ ] Implement airtime bypass for emergency-flagged packets
- [ ] Unit tests for packet construction/parsing

### Phase 2: State Machine & Activation (Est. 2-3 days)
- [ ] Implement `emergency.c` state machine (IDLE → ACTIVE → CANCELLING → IDLE)
- [ ] Button long-press detection (3s) with debounce
- [ ] Beacon scheduler (30s initial, 2min steady-state)
- [ ] Auto-timeout at 24 hours
- [ ] LED/buzzer confirmation feedback

### Phase 3: Relay & Tracking (Est. 2 days)
- [ ] `emergency_relay.c` — priority relay with TX power boost
- [ ] `emergency_tracker.c` — active emergency table (8 entries, LRU)
- [ ] Extended hop_limit logic
- [ ] Extended dedup window (300s) for emergency packets

### Phase 4: Cancellation & Auth (Est. 2 days)
- [ ] Ed25519 signature over cancel payload
- [ ] Signature verification on cancel receipt
- [ ] Cancel propagation and tracker update
- [ ] Post-cancel 15-minute cooldown

### Phase 5: GPS & Power (Est. 1-2 days)
- [ ] GPS integration — live fix, stale position, no-GPS fallback
- [ ] Power management overrides — no sleep, max TX, continuous RX
- [ ] Battery estimation and low-battery degradation logic

### Phase 6: BLE & UI (Est. 2-3 days)
- [ ] BLE characteristics (activate, notify, position inject)
- [ ] OLED emergency alert screens (own emergency + received alerts)
- [ ] Companion app notification flow

### Phase 7: Simulator & Testing (Est. 2 days)
- [ ] Simulator scenarios A–G implementation
- [ ] Integration tests on hardware (Heltec V3 + T-Beam)
- [ ] Range test with emergency hop extension

**Total estimate: ~14-18 days**

---

## Open Questions

1. **Should distress beacons also be signed?** Current decision is no (cost vs. risk tradeoff). Revisit if spoofed distress becomes a real problem.
2. **Emergency channel encryption.** Currently proposed as plaintext-with-signing. Alternative: well-known PSK that ships with firmware. PSK provides minimal security but prevents non-Bramble devices from decoding. Leaning toward plaintext for maximum interop.
3. **Cross-network bridging.** If a Bramble node has internet connectivity (WiFi/cellular via companion), should it forward emergencies to a web service? Useful but out of scope for v1.
4. **Regulatory.** Max TX power (+22 dBm) and duty cycle exemption may conflict with regional regulations (EU 868MHz 1% duty cycle). Need per-region compliance check. Emergency exemption may not be legally recognized for ISM band.
