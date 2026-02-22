# Bramble Idle Airtime A/B Report — Beacon 30s vs 60s

Date: 2026-02-22
Window: 300s idle captures

## Capture Files

### Baseline (beacon 30s firmware)
- `/home/user/src/bramble/reports/idle-capture-2026-02-22/heltec-63929F02-idle-300s.jsonl`
- `/home/user/src/bramble/reports/idle-capture-2026-02-22/tdeck-04CAAAF8-idle-300s.jsonl`

### Variant (beacon 60s firmware)
- `/home/user/src/bramble/reports/idle-capture-2026-02-22/heltec-63929F02-idle-300s-beacon60.jsonl`
- `/home/user/src/bramble/reports/idle-capture-2026-02-22/tdeck-04CAAAF8-idle-300s-beacon60.jsonl`

## Key Result (Most Reliable Comparison)

T-Deck baseline capture at 30s was incomplete due prior WS instability (only 2 events), so the strongest apples-to-apples comparison is **Heltec node before vs after**:

- Heltec @ 30s: 21 beacon RX events, 6.345s est airtime / 300s
- Heltec @ 60s: 16 beacon RX events, 4.853s est airtime / 300s

Delta (Heltec):
- Events: **-23.8%**
- Estimated airtime: **-23.5%**

## Observed Traffic Composition (Both Arms)

- Category: beacon = 100%
- Tier/bucket: broadcast = 100%
- Packet type: 0x05 beacon = 100%

No chat/user traffic observed in these windows.

## Interpretation

Idle airtime burn is dominated by periodic beacon broadcast traffic. Increasing beacon interval from 30s to 60s materially reduced observed idle beacon load on the reliable node sample.

## Caveats

- Baseline T-Deck 30s capture was under-sampled (WS session issue), so fleet-wide percentage reduction should be treated as preliminary.
- Third active peer was not captured as a separate endpoint in this A/B pass.

## Recommended Next Actions

1. Keep 60s beacon interval and run a longer 15–30 minute capture on all reachable nodes.
2. Add a dedicated per-node TX counter/field in traffic debug monitor output to separate local TX from RX-only observation.
3. Bring third peer into LAN WS capture set (or capture over serial with matching schema).
4. If 60s remains stable, implement adaptive beacon interval policy (short when topology changes, long when stable).
