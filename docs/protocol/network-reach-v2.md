# Network Reach v2 (Sweep Aggregation)

Status: implemented design target

## Purpose
Network Reach answers: "Which peers are reliably reachable from this node right now?"

v2 uses a **bounded 3-round sweep** per user probe, then reports an aggregated final result to reduce single-shot RF noise without unbounded airtime.

## Wire/runtime behavior (v2)

- `bramble.sendProbe` returns:
  - `probe_id` (hex string)
  - `ack_window` (seconds, currently `5`)
  - `rounds_total` (currently `3`)
- Sweep behavior:
  - Round 1 sent immediately
  - Rounds 2-3 sent with short fixed spacing (`PROBE_SWEEP_INTERVAL_MS`, currently 350 ms)
- During collection, firmware may emit `bramble.onProbeResult` (incremental updates).
- At collection end, firmware emits `bramble.onProbeComplete` with:
  - `probe_id`, `unique_count`, `duration_ms`, `rounds_total`
  - `responders[]` aggregated by responder address:
    - `address`, `hops`, `rssi`, `snr`, `latency_ms`, `seen_rounds`

## Aggregation rules

Per responder (key: `responder_addr`):
- upsert (never append duplicates)
- keep latest latency
- keep best quality sample (max RSSI / max SNR)
- track `seen_rounds` via per-round bitmask in firmware

## Confidence model

UI confidence is derived from `seen_rounds / rounds_total`:
- `3/3`: high confidence (stable)
- `2/3`: medium confidence
- `1/3`: low confidence (likely edge/noisy link)

## Invariants

1. No self responder.
2. One logical row per responder per probe.
3. Completion is explicit and bounded by collection window.
4. Mixed-fleet compatibility:
   - older responders without round metadata are treated as seen in round 1
   - final UI still renders deterministic aggregated rows.

## Airtime bounds

Per user probe:
- Origin probe TX: 3 total (one per round)
- Responder ACK retries remain bounded (current behavior: 3 ACK sends per received probe)
- Collection window fixed (5 s)

This keeps airtime deterministic while materially improving reach consistency versus single-shot sampling.
