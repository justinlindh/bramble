# Network Reach E2E Checklist (v2 Sweep)

## Objective
Validate that 3-round sweep aggregation improves consistency while staying bounded in airtime and preserving deterministic UI output.

## Preconditions
- Firmware includes sweep mode + aggregated `onProbeComplete.responders[]` with `seen_rounds`.
- Webapp renders confidence (`seen_rounds/3`) and dedupes by responder.
- Temporary probe debug spam removed.

## Test plan

### A) Baseline consistency (before/after)
1. Pick origin node (example: `63929F02` / `192.168.1.21`) in a 3-node mesh.
2. Run N probes (recommend N=10+), capturing trace output (`scripts/probe-trace.py`).
3. Compute per-run responder count and responder set.
4. Repeat with sweep mode enabled.

Expected: lower variance in responder count/set post-sweep.

### B) Confidence quality check
1. For each responder in final results, verify confidence shown as `seen_rounds/3`.
2. Confirm responders seen across more rounds generally have more stable presence across repeated probe runs.

Expected: confidence aligns with observed stability.

### C) Backward compatibility
1. Include at least one older node that does not send round metadata.
2. Run probe.

Expected: node still appears (typically `1/3` confidence), no parsing failures.

### D) Airtime sanity
1. Verify origin sends exactly 3 probe TX per user trigger.
2. Verify collection completes in fixed window (5s currently).

Expected: bounded, predictable airtime and completion timing.

### E) Heltec V4 GNSS bring-up gate (when origin or responders include Heltec V4)
1. Run the board bring-up flow in `docs/heltec-v4-gnss-bringup.md` (build/flash/monitor + status checks).
2. Confirm `bramble.getStatus` returns `"hardware":"heltec_v4"` on V4 node(s).
3. Confirm sweep probe behavior remains stable with V4 in the mesh (no panics, completion observed).
4. If GNSS UART mapping is still pending in board config, record GNSS validation as pending (do not claim fix acquisition pass).
5. If GNSS UART mapping is enabled and validated, run `bramble.getGpsPosition` checks from the bring-up doc and record first-fix timing.

Expected:
- Network reach sweep behavior remains valid with Heltec V4 present.
- GNSS claims are marked according to actual hardware validation state.

## Evidence to attach in wrap-up
- Trace artifacts (`tmp/probe-trace-*.jsonl`)
- Before/after table for N runs:
  - mean responders
  - min/max responders
  - % runs with full expected set
- Example per-origin output for all 3 nodes.
- If Heltec V4 used: attach bring-up evidence per `docs/heltec-v4-gnss-bringup.md`.

## Pass criteria
- No self responders.
- No duplicate rows per responder.
- Completion always observed.
- Confidence rendered and coherent with trace evidence.
- Consistency metrics improve vs single-shot baseline.
- If Heltec V4 participates: `"hardware":"heltec_v4"` observed in status evidence.
- GNSS acceptance marked pass only when hardware-validated; otherwise explicitly marked pending.
