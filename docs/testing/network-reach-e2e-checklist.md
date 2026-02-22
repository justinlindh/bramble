# Network Reach E2E Checklist (v2 Sweep)

## Objective
Validate that 3-round sweep aggregation improves consistency while staying bounded in airtime and preserving deterministic UI output.

## Preconditions
- Firmware includes sweep mode + aggregated `onProbeComplete.responders[]` with `seen_rounds`.
- Webapp renders confidence (`seen_rounds/3`) and dedupes by responder.
- Temporary probe debug spam removed.

## Test plan

### A) Baseline consistency (before/after)
1. Pick origin node (example: `63929F02` / `192.0.2.0`) in a 3-node mesh.
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

## Evidence to attach in wrap-up
- Trace artifacts (`tmp/probe-trace-*.jsonl`)
- Before/after table for N runs:
  - mean responders
  - min/max responders
  - % runs with full expected set
- Example per-origin output for all 3 nodes.

## Pass criteria
- No self responders.
- No duplicate rows per responder.
- Completion always observed.
- Confidence rendered and coherent with trace evidence.
- Consistency metrics improve vs single-shot baseline.
