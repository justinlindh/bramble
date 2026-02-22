# Network Reach v2 (Design Invariants)

Status: draft (test-driven scaffolding)

## Purpose
Network Reach answers: "Which peers are reachable from this node right now?"

It uses active probing and ACK collection with strict invariants so UI results are stable and meaningful.

## Invariants

1. **No self responder**
   - A probe result must never include the origin node as a responder.

2. **One logical row per responder per probe**
   - Result identity key is `(probe_id, responder_addr)`.
   - Duplicate ACKs from the same responder must update that row, not append a new row.

3. **Quality merge rule**
   - For duplicate ACKs from same responder:
     - keep latest latency
     - keep best RSSI/SNR quality sample (higher RSSI / SNR preferred)

4. **Bounded completion semantics**
   - A probe session has explicit completion (`complete=true`) after collection window expires.
   - After completion, additional ACKs for that probe are ignored.

5. **Compatibility note**
   - Mixed fleets may still emit duplicate notifications over transport; collectors must normalize using responder key.

## Reference keys
- Probe session key: `(origin_addr, probe_id)`
- Responder row key: `(probe_id, responder_addr)`

## Non-goals (v2)
- Perfect multi-hop topology reconstruction.
- Route path attribution beyond hop count/quality for each responder.
