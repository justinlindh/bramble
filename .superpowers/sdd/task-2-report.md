# Task 2 Report: Flooded-ACK (sender-confirmation without routes)

Status: DONE

## What shipped

Under `s_flood_transport`, a unicast message's confirmation (the ACK in
firmware, the delivery receipt in gosim, both feeding `confirmed_delivery_rate`)
now FLOODS back to the original sender through the SAME engine the DATA flood
uses, instead of a route-lookup forward. No route table is consulted on the
confirmation-return path. Toggle OFF is byte-for-byte the old reactive behavior.

## How the ACK floods (which machinery is reused)

- Origination is unchanged: the destination's `send_ack` puts one authenticated
  ACK on the air (`mesh_tx(TX_KIND_ACK)`). On a real LoRa medium every neighbor
  hears it, exactly like a broadcast origination. No change needed at the two
  `handle_data` delivery sites or in `send_ack`.
- Relay is the change. `handle_ack`, when the ACK is NOT for this node and
  `s_flood_transport` is on, no longer calls `forward_ack` (route_lookup).
  Instead it runs the identical flood dance the DATA flood uses:
  `channel_flood_decide(hop_limit, is_own_echo, budget_permits, esp_random())`
  gates the rebroadcast on hop-limit, the own-echo duplicate guard, and the
  ACK-lane airtime budget (`tx_gate_check(..., TX_KIND_ACK)`); on relay it
  appends its address to `relay_path`, decrements `hop_limit`, re-serializes,
  and schedules a jittered rebroadcast on the shared `schedule_flood_relay`
  queue.
- Dedup + suppression reuse the existing engine. Flooded-ACK copies share
  `header.packet_id`, so the existing dispatch `s_dedup` gate
  (`packet_id ^ type`) already dedups copies 2+ before they reach `handle_ack`.
  Those duplicate copies hit the same dispatch-gate suppression path the DATA
  flood added: a new `PKT_TYPE_ACK` branch recomputes the src-qualified flood
  key (`header.packet_id ^ big-endian ack.src_addr`) and calls
  `channel_flood_note_overheard`, so `FLOOD_SUPPRESS_AFTER = 2` cancels a
  pending ACK relay once two other copies are overheard, exactly as for DATA.
- One shared relay queue, correct per-lane airtime. `schedule_flood_relay` and
  `pending_flood_relay_t` gained a `tx_kind` field so a flooded DATA still
  debits `TX_KIND_DATA_BROADCAST` and a flooded ACK debits `TX_KIND_ACK`
  (CRITICAL lane), with one queue and one suppression engine.

## MAC-before-relay gate (authenticated flood) + its non-vacuous test

`handle_ack` already calls `ack_verify` (network-key MAC over
`src_addr || ack_packet_id || seq`) BEFORE reaching the not-for-us branch, so a
bad-MAC ACK is dropped and never enters the flood relay. This is the same
"never act on unauthenticated wire bytes" rule the DATA flood enforces via
`data_auth_verify`. No unverified ACK is ever rebroadcast or consumed.

Host test `test/test_flooded_ack.c` proves it non-vacuously:
- `test_flood_ack_relay_valid`: a correctly signed ACK not addressed to the
  relay DOES flood (relayed == true).
- `test_flood_ack_bad_mac_never_relays`: the SAME frame with one flipped
  auth_hmac byte on the wire fails `ack_verify` and is NOT relayed. The only
  difference from the valid case is the MAC, so the drop is real, not vacuous.
- `test_flood_ack_wrong_key_never_relays`: an ACK signed under an attacker's
  network key fails verification under the real key and is not relayed.

## How the sender correlates a flooded ACK to a pending message

Unchanged and route-free. When the flooded ACK reaches the original sender
(`ack.header.dest_addr == self`), `handle_ack` consumes it via the existing
`pending_ack_remove(ack.ack_packet_id)` + `msg_store_update_status_with_route`,
marking the message DELIVERED-confirmed. `ack_packet_id` is the DATA's
packet_id carried in the ACK, the correlation id. A re-ACK of a duplicate DATA
draws a FRESH `header.packet_id` (via `next_packet_id`), so it is not deduped
and floods anew, preserving the Phase 1 re-ACK-on-duplicate second chance.
`test_flood_ack_sender_consumes_marks_confirmed` proves the consume + confirm
(and idempotence on a duplicate copy);
`test_flood_ack_sender_no_match_no_confirm` proves a non-matching
`ack_packet_id` confirms nothing.

## gosim empty-route-table confirmation proof

gosim's bridge.c uses the delivery receipt as its confirmation packet (it feeds
`confirmed_delivery_rate` exactly as the firmware ACK does). Under
`flood_transport` the receipt now FLOODS: the destination broadcasts it once,
and `_handle_delivery_receipt` flood-relays it via the same `bridge_flood_relay`
engine (src-qualified dedup + `channel_flood_decide` + jittered relay +
suppression) instead of `forward_data` route lookup. The originator consumes it
(`bridge_msg_track_confirm`).

`simulator/gosim/flooded_ack_test.go`, driven through the REAL firmware flood
engine on the 3-hop A-B-C-D line (D is 3 hops from A, out of direct range):
- `TestFloodedAckConfirmsSenderWithoutRoutes`: D floods the receipt back; every
  DELIVERY_RECEIPT `packet_sent` is a broadcast (dest `0xFFFFFFFF`), so no route
  lookup produced any of them; A observes the confirmation (a `message_delivered`
  at node A) and `confirmed >= 1` (confirmed_delivery_rate registers). Observed
  the receipt flooding D -> C -> B -> A, all `0xFFFFFFFF`.
- `TestFloodedAckOffRoutesReceipt` (A/B baseline): identical topology with the
  toggle off returns the receipt as a routed unicast (dest == a specific next
  hop) and still confirms. The broadcast-vs-unicast asymmetry is the toggle's
  own proof that the confirmation transport changed from routed to flooded.

Honest scope note: send-side origination still uses reactive discovery (that is
Task 3), so forward-path route entries exist; the Task 2 deliverable is that the
CONFIRMATION-return path uses zero route lookups, proven by the flooded
(broadcast) receipts. This mirrors the Task 1 test's identical framing.

## CI (all green)

- Host suite: 94/94 (was 93; +test_flooded_ack, 9 cases).
- gosim: `go build ./...` clean, `go test -count=1 ./...` ok (+2 Task 2 cases).
- cppcheck (runner image, warning/performance/portability, -std=c11): exit 0.
- clang-format v14 (runner image): touched files stable/idempotent. (Local
  strict check flags only the untouched `main/lv_malloc_psram.c`, a pre-existing
  local-v22 vs CI-v14 discrepancy, not in this diff.)
- check-rpc-contract.sh: OK (52 methods; no RPC surface changed in Task 2).
- Board build (heltec-v3, ESP-IDF v5.4.1): Project build complete, signed image
  generated.

## Files changed

- `main/mesh_task.c`: handle_ack flood-relay branch; dispatch-gate ACK
  suppression note; `schedule_flood_relay` + queue gain `tx_kind`.
- `components/routing/include/channel_flood.h`: `tx_kind` on
  `pending_flood_relay_t`.
- `simulator/gosim/bridge.c`: flood the delivery receipt (origination +
  relay) under flood transport; derive flood-relay pkt_type from the wire.
- `simulator/engine/sim_emitter.c`: label `PKT_TYPE_DELIVERY_RECEIPT` in
  `pkt_type_name` so scenarios can distinguish a flooded vs routed receipt.
- `test/test_flooded_ack.c` (+ CMakeLists entry): host proof.
- `simulator/gosim/flooded_ack_test.go`: gosim system proof.

## Concerns / follow-ups

- Flooded receipts in gosim carry an empty relay_path (the flood engine does
  not grow it), so a confirmed message's UI "path" is empty under flood
  transport. Confirmation itself is unaffected. A per-hop trace for flooded
  confirmations, if wanted, is a later cosmetic item.
- Suppression only bites when the jitter window exceeds frame airtime (the
  carried Task 1.5 constraint): at slow PHYs airtime > jitter so every node
  relays. Measure Task 4 at SF7-dense, per the ledger note.
