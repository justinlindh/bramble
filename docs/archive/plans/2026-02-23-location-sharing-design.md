# Privacy-First Location Sharing Design (GPS + Non-GPS)

Date: 2026-02-23
Status: Approved

## Goals

- Enable robust location sharing for GPS-enabled devices (T-Deck first) and non-GPS devices (manual fallback).
- Keep sharing strictly opt-in (default OFF).
- Preserve privacy by default (default precision = coarse).
- Persist all sharing settings across restarts.
- Support hybrid recipient model:
  - trusted contacts allowlist
  - selected channels for map/coverage contribution

## Product Decisions (Approved)

- Recipient model: **Hybrid** (contacts + selected channels)
- Default precision when enabled: **Coarse**
- Coverage update cadence: **Periodic**
- Session duration: **Persistent until manually disabled**
- Transport architecture: **Dedicated location packet handling** (`PKT_TYPE_LOCATION`), not chat-message JSON piggybacking

---

## High-Level Architecture

### 1) Location Policy Engine (firmware)

A dedicated policy engine decides if/when/what to share.

Inputs:
- persisted config (`bramble_loc` NVS namespace)
- source position (GPS preferred; manual fallback)
- current time and last-send timestamps
- recipient targets (contacts + channels)

Outputs:
- serialized tiered location payloads
- send decisions per target scope

Core gates:
- sharing enabled?
- valid source available?
- interval/rate-limit satisfied?
- target set non-empty?
- policy privacy constraints satisfied?

### 2) Dedicated Location Transport

Use first-class location packet path:
- packet type: `PKT_TYPE_LOCATION`
- payload: tiered binary format (full/coarse/presence) using existing `components/location` serializers
- encryption and mesh routing through existing packet pipeline

### 3) Receive + Cache Integration

`mesh_task` handles inbound `PKT_TYPE_LOCATION`:
- validate/decrypt
- parse tier payload
- cache via `location_cache_update`
- expose through RPC and UI

### 4) Shared Control Plane

T-Deck UI and web client use the same RPC methods/policy model.
- T-Deck gets on-device controls
- Web client provides complete controls for nodes without rich local UI

---

## Data Model and Persistence

NVS namespace: `bramble_loc`

Required persisted fields:
- `enabled` (u8 bool) — default `0`
- `source` (str enum: `gps|manual|auto`) — default `auto`
- `def_tier` (str enum: `full|coarse|presence`) — default `coarse`
- `interval_s` (u16) — default e.g. `300`
- `last_sent_ts` (u32) — updated at runtime
- `channel_mask` (bitmask or compact list) — selected sharing channels
- per-contact tier entries: `lc_<addr>` -> tier
- optional manual fallback: `lat_e6`, `lon_e6`

Boot behavior:
- load policy from NVS
- initialize engine with persisted values
- if `enabled=1`, schedule periodic sharing loop with current policy

---

## Sender Behavior

1. Periodic timer tick (config interval)
2. Resolve source position:
   - if GPS available + valid fix -> GPS
   - else if manual configured -> manual
   - else skip and log reason
3. Resolve targets:
   - contacts allowlist
   - selected channels
4. For each target/scope, apply tier policy and serialize payload
5. Emit `PKT_TYPE_LOCATION`
6. Update `last_sent_ts`

Safety checks:
- interval floor enforced (no spam)
- disabled policy emits nothing
- if no valid targets: emit nothing

---

## Receiver Behavior

On inbound `PKT_TYPE_LOCATION`:
- authenticate/decrypt via normal packet flow
- deserialize by declared tier
- update peer cache with timestamp/freshness
- trigger UI/RPC observers

RPC query (`getPeerLocations`) should return:
- self location (if enabled and available)
- cached peers with tier/freshness metadata

---

## UI/UX Requirements

### T-Deck UI

Settings screen must include:
- Sharing toggle (OFF by default)
- Precision tier selector (default coarse)
- Interval selector
- Source selector (`auto/gps/manual`)
- Target editor:
  - contacts list with per-contact tier
  - channels included for coverage map contribution
- status row:
  - sharing ON/OFF
  - last share timestamp
  - active source (gps/manual)
- “Panic off” one-tap disable

### Web Client

Equivalent controls for all nodes, especially simple-screen hardware:
- full policy editor
- contacts/channels target management
- readable preview (“Sharing coarse every 10m with X contacts and Y channels”)
- reboot-persistence confirmation indicators

---

## Privacy Model

- Strict opt-in; default disabled
- Coarse default on first enable
- Explicit recipient scope (no implicit expansion)
- Min interval/rate limiting
- Local transparency (last-share info and current policy)
- Panic off immediate disable

Non-goals (for this phase):
- global anonymity guarantees against wide-area traffic analysis
- automatic audience widening based on dynamic channel discovery

---

## Compatibility / Migration

- Introduce dedicated location packet handling now as primary path.
- Keep temporary legacy fallback parse for prior JSON location messages for one release window if needed.
- Migrate `shareLocationOnce` to send dedicated location packet (not text payload).

---

## Testing and Verification

### Host/unit
- policy engine decision tests (enabled/disabled, interval, source validity)
- serializer/deserializer tests for full/coarse/presence
- cache update/freshness tests
- recipient resolution tests (contacts vs channels hybrid)

### Integration/device
- T-Deck GPS enabled flow:
  - sharing disabled => no outbound location packets
  - enable sharing => periodic `PKT_TYPE_LOCATION` begins
  - reboot => settings preserved and behavior restored
- non-GPS node manual fallback flow
- mixed-network verification across 3 devices

### Privacy regression tests
- opt-in default remains OFF after fresh flash
- coarse quantization correctness
- channel/contact scoping correctness

---

## Implementation Sequence (recommended)

1. Implement policy engine + persisted config schema
2. Add dedicated `PKT_TYPE_LOCATION` tx path
3. Add rx handling + cache integration in `mesh_task`
4. Update RPC methods (`shareLocationOnce`, `getPeerLocations`, config methods)
5. T-Deck UI controls
6. Web client controls
7. Compatibility fallback + migration notes
8. Full host + hardware verification

---

## Success Criteria

- Opt-in location sharing works on GPS and non-GPS devices.
- T-Deck can enable/configure sharing on-device.
- Web client can fully configure sharing for all devices.
- Location settings survive reboot.
- Privacy defaults are enforced (OFF + coarse when enabled).
- Dedicated location packet path is used in production flow.
