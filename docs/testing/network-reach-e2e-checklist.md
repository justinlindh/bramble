# Network Reach E2E Checklist (v2)

## Objective
Validate that Network Reach reports **unique reachable responders** per probe, excludes self, and completes cleanly.

## Preconditions
- Firmware includes:
  - responder upsert semantics (one row per responder)
  - self-responder filter
  - `bramble.onProbeComplete` notification
  - `bramble.sendProbe` returns `ack_window`
- Webapp includes probe-result normalization by responder address.

## Test matrix

### A) Basic single-hop (strong links)
1. Run probe from Node A.
2. Confirm `onProbeResult` notifications for each reachable peer.
3. Confirm each responder appears once in UI (no duplicate rows).
4. Confirm no self row.
5. Confirm probe exits collecting state via `onProbeComplete`.

Expected: stable unique responder list, no self, no duplicate rows.

### B) Mixed firmware compatibility
1. Probe from a v2 node with at least one older node in mesh.
2. Confirm results still normalize in webapp.

Expected: no duplicate UI rows; completion still works (event or timeout fallback).

### C) Weak/asymmetric link
1. Place one peer at marginal RSSI.
2. Probe repeatedly (5 runs).
3. Compare unique responders per run.

Expected: weak peer may intermittently fail, but when received it appears once and never as self.

### D) From each node
1. Probe from each node in 3-node mesh.
2. Record unique responder set each time.

Expected: topology asymmetry may differ by origin, but invariants hold for every origin.

## Quick verification commands

### WS probe (example)
Use existing helper script:

```bash
python3 probe_call.py 192.168.1.21
```

### Serial sanity
- `bramble.getVersion`
- `bramble.getStatus`
- `bramble.getNeighbors`

## Pass/fail criteria
- PASS if all invariants hold:
  - no self responders
  - no duplicate responder rows
  - completion observed
- FAIL if any invariant breaks.

## Notes from current environment
- Node `192.168.1.21` currently reports a single responder (`04CAAAF8`) in repeated runs despite seeing 2 neighbors; this indicates likely RF/asymmetric reachability behavior for that origin, not a duplicate/self normalization bug.
