# Phase 2 report: signed identity-attestation frame + origination

Status: DONE. Branch feat/ed25519-identity, base 5e0c1cb3 (Phase 1 report).

## Commits

- 673e6322 feat(packet): identity attestation frame with canonical signed message
- acc67e32 feat(mesh): originate signed identity attestations at boot and on cadence

## Wire layout (144 bytes, fixed)

PKT_TYPE_IDENTITY_ATTESTATION = 0x15 (0x14 = LOCATION was the previous top).

| offset | size | field |
|---|---|---|
| 0 | 12 | bramble_header_t (version=4, type=0x15, flags=0, hop_limit, dest=0xFFFFFFFF, packet_id) |
| 12 | 4 | src_addr, big-endian (put_be32, matching every other frame body) |
| 16 | 32 | x25519_pub |
| 48 | 32 | ed25519_pub |
| 80 | 64 | sig (Ed25519) |

IDENTITY_ATTESTATION_SIZE = HEADER_SIZE + 4 + 32 + 32 + 64 = 144.
Serializer follows packet.c's put_be32/memcpy fixed-offset style; the
deserializer enforces len == 144 EXACTLY (both truncated and padded frames
rejected; pinned by test).

## Signed bytes (the security core)

sig = crypto_ed25519_sign(node's own ed25519_private_key) over the
canonical message built by bramble_identity_attestation_signed_msg
(components/packet/packet.c, so signer and Phase 3 verifier share one
implementation):

    "bramble-ident-v1" (16 bytes, no NUL)
    || src_addr (4, big-endian)
    || x25519_pub (32)
    || ed25519_pub (32)
    = 84 bytes (IDENTITY_ATTESTATION_MSG_SIZE)

No header fields in the message (hop_limit relay-mutable, packet_id
per-send); a test mutates every header field and asserts the message
bytes are bit-identical. Domain separation via the context prefix.

Deliberately NO network-key MAC on this frame: it is self-authenticating
(any member can check the Ed25519 sig against the embedded ed25519_pub,
no shared secret needed); the network key gates relay policy, not the
claim's truth. Documented in the struct comment in packet.h as required.

## Origination (main/mesh_task.c)

- send_identity_attestation(): builds from s_identity (address =
  src_addr, public_key = x25519_pub, ed25519_public_key = ed25519_pub),
  signs with s_identity->ed25519_private_key, serializes, transmits.
  hop_limit = flood_origination_hop_limit(s_flood_transport,
  s_flood_hop_limit) -- ROUTE_HOP_LIMIT_MAX reactive, the configured
  flood hop budget under flood transport, identical to
  send_data_packet/send_ack. packet_id = next_packet_id().
- Boot hook: attempt_identity_attestation(now_ms()) immediately after
  the first post-jitter send_beacon() in mesh_task's boot sequence
  (radio confirmed up, jitter already spread the fleet).
- Periodic hook: mesh_periodic_maintenance fires it every
  ATTESTATION_INTERVAL_MS (15 min, named constant) once the boot send
  arms the schedule; a failed/denied send retries after
  ATTESTATION_RETRY_MS (60 s) instead of waiting a full interval.
- Bonus (brief's optional c): identity regeneration after an address
  collision (handle_beacon) re-announces immediately, since the old
  attestation no longer describes the node.

## Budget lane

TX_KIND_DATA_BROADCAST via mesh_tx -> tx_gate. That debits
AIRTIME_TIER_BROADCAST, the same tier TX_KIND_BEACON and the flood
relays use, WITHOUT eating the beacon reserve carve-out (tx_gate.c holds
one beacon ToA back from non-beacon broadcast spenders, which is exactly
the right relationship: attestations must never starve beacons). 144 B
every 15 min is the approved cadence; constant comment flags that raising
it needs a budget re-flag.

## Relay behavior STATEMENT (verified, not guessed)

An unknown-type broadcast frame does NOT relay: neighbors-only this
phase. Verified by reading mesh_process_rx_packet's dispatch
(main/mesh_task.c): the channel-flood relay path is entered only from
TYPE-specific cases (PKT_TYPE_DATA -> handle_data/channel_flood_decide,
PKT_TYPE_ACK -> handle_ack's flood branch). Every other type falls to
the switch's `default:` case, which logs "Unhandled packet type" at
debug level and drops. There is no generic broadcast relay path to
trivially hook, so per the brief this phase documents neighbors-only;
Phase 3 extends relay alongside receiver verification. Same-version v4
peers without the handler drop the frame safely at that default case
(the additive-no-version-bump premise, confirmed), after a harmless
dedup-table insert (packet_id ^ type<<24).

## gosim

No model change needed and none made. simulator/gosim/bridge.c's RX
switch already ends in an inert `default: break;`, so an attestation
frame would be counted as a reception and ignored. gosim never
originates one (bridge.c drives components directly, not mesh_task.c).
go build + go test -count=1 ./... green.

## Tests (test/test_identity_attestation.c, 13 cases)

- wire size = 144; serialize writes exact offsets (12/16/48/80 pinned
  byte-for-byte); full round-trip equality.
- serializer rejects short buffer; deserializer rejects 143, 145, 0 and
  accepts exactly 144.
- canonical message: exact 84 bytes pinned (context string, BE src_addr,
  pub offsets); header-independence (all four header fields mutated,
  message identical); short-buffer rejection.
- signature verifies after a wire round trip using only frame contents
  (the Phase 3 verifier's exact procedure).
- tamper-fails-verify, non-vacuous (untampered control verified first in
  the same case): src_addr, x25519_pub, ed25519_pub each flipped ->
  verify false; tampered sig -> false; wrong (unrelated) pubkey -> false.
- origination contract: bramble_identity_t -> frame mapping identical to
  send_identity_attestation's, signed by the identity's own key,
  verifies after round trip (mesh_task.c is never host-compiled, so this
  is the host-side origination pin).

TDD: suite written first and confirmed red (undeclared type/symbols),
then implementation, then green.

## CI (all green)

- Host: bash test/run_all_tests.sh -> 98/98 suites pass (new suite included).
- Board: make ci-quality-board-build (heltec-v3, ESP-IDF) -> Done!
- clang-format v14 (runner image, strict): PASS, 372 files.
- cppcheck (runner image, CI flags): clean.
- check-rpc-contract: OK, 53 methods (no RPC change, as expected).
- gosim: go build + go test -count=1 ./... -> ok.

## Concerns / notes for Phase 3

- Reach is neighbors-only until Phase 3 adds the relay; at 15-min
  cadence a multi-hop mesh converges slowly on identities until then.
- The attestation is replayable as-is (no seq/timestamp in the signed
  bytes; an old frame is still a true statement of the binding). Phase 3
  pinning should treat it as idempotent state, not an event, and the
  address-rebind design must decide what freshness means.
- Unknown-type frames (including attestations at un-upgraded peers)
  still consume a dedup slot; negligible at this cadence.
