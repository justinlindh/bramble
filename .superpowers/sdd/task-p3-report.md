# Phase 3 report: attestation relay gate, flood relay, verified TOFU pinning

Branch: feat/ed25519-identity. Base for this phase: e317ea34 (Phase 2 report).
Status: DONE. Full CI green (host, board, clang-format v14 + cppcheck in the
runner image, check-rpc-contract, gosim -count=1).

## Commits

- 2bd007d8 feat(packet): relay-gate MAC + origin seq on the identity
  attestation frame
- e0e87356 feat(identity): verified TOFU pin store with
  impersonation-conflict detection
- 360d7ffc feat(mesh): flood-relay identity attestations behind the cheap
  MAC gate
- c8089cc7 feat(gosim): identity attestations through the real relay path
  + conflict scenario

## Part A: extended wire frame (144 -> 158 bytes)

    header(12) | src_addr(4) | x25519_pub(32) | ed25519_pub(32) | sig(64)
              | auth_hmac(8) | seq(6)

Absolute offsets: src 12, x25519 16, ed25519 48, sig 80, auth_hmac 144,
seq 152. Exact-length deserializer (158, trailing garbage = truncation =
reject), pinned byte-for-byte by test_identity_attestation.c.

- auth_hmac: network_key_mac with context "bramble-ident-relay-v1" via the
  new ident_relay_sign/ident_relay_verify pair in components/routing_auth
  (exact same pattern as ack/receipt/rerr/data_auth: build_auth_buf helper,
  8-byte MAC, constant-time compare).
- MAC coverage: src_addr(4, BE) || x25519_pub(32) || ed25519_pub(32) ||
  sig(64) || seq(6) = 138 bytes. NOT the header: hop_limit is the one
  relay-mutated field, so the MAC survives every hop and relays pass the
  frame through byte-identical except that decrement.
- Division of labor (documented in packet.h's struct comment and
  routing_auth.h): the Ed25519 sig carries the identity claim's TRUTH
  (self-authenticating, keyless-verifiable); the MAC gates RELAY PRIVILEGE,
  preserving the branch invariant that keyless traffic never propagates
  and that outsiders cannot grind relays' Ed25519 verifies.
- seq: 48-bit control seq drawn ONCE at origination via control_seq_next
  (the send_ack pattern, fail-closed: no seq, no attestation), written
  before ident_relay_sign (MAC covers it), NEVER re-drawn by relays.
  Origination order in send_identity_attestation: canonical msg -> Ed25519
  sign -> seq -> ident_relay_sign -> serialize.
- Canonical SIGNED bytes are UNCHANGED from P2 ("bramble-ident-v1" ||
  src_addr || x25519_pub || ed25519_pub); P2's signed-msg tests pass
  unmodified except the frame-size constant, and a new assertion pins that
  auth_hmac/seq do NOT feed the signed message.

## Part B: relay path (mesh_process_rx_packet)

New PKT_TYPE_IDENTITY_ATTESTATION case (was default-dropped) ->
handle_identity_attestation, in this order:

1. exact-length deserialize;
2. ident_relay_verify FIRST (cheap MAC). Fail = drop: no relay, no pin,
   no Ed25519 verify, no replay-window touch (a bad-MAC frame cannot
   pre-burn a victim's seq);
3. control_replay_ok(src_addr, seq), both MAC-covered. Kills re-injection
   with a rewritten packet_id (packet_id is header, not MAC-covered, so
   dedup alone cannot);
4. flood dedup on s_flood_dedup with the established src-qualified key
   (packet_id ^ src_addr);
5. deliver to identity_store_handle_attestation REGARDLESS of the relay
   decision;
6. relay through the SHARED flood engine: channel_flood_decide (hop_limit
   > 1, dup/own-echo suppression, tx_gate_check on the BROADCAST lane) +
   schedule_flood_relay jittered queue, TX_KIND_DATA_BROADCAST. Frame
   rebroadcasts UNMODIFIED except hop_limit. No second flood
   implementation.

The dispatch dedup gate also gained the attestation variant of the
overheard-copy suppression bookkeeping (channel_flood_note_overheard),
mirroring the flooded-ACK block including its MAC-before-counting rule.

Relays NEVER Ed25519-verify; only pinning receivers do (the one verify
lives inside identity_store_handle_attestation).

## Part C: pin/conflict semantics (components/identity/identity_store.{h,c})

Caller-owned identity_store_t (no singleton: firmware has s_identity_pins
in mesh_task.c, gosim one per node, tests their own).

- Delivery path: ignore self (att.src_addr == own address), verify Ed25519
  over the canonical message against the frame's OWN embedded key, then
  TOFU-pin.
- Pin: not pinned -> store (INFO "Identity pinned"). Pinned identical
  (both keys) -> idempotent refresh of last_confirmed_ms only (no churn).
  Pinned DIFFERENT keys -> CONFLICT: rejected, original binding untouched
  (including its LRU position: a conflict is not a confirmation), WARN log
  + conflicts counter. sig_failures counts MAC-valid-but-sig-invalid
  deliveries (keyed-member garbage) with a WARN.
- Bounds: 32 entries (neighbor-table scale), LRU eviction by
  last_confirmed_ms (re-heard identical attestation refreshes; the least
  recently confirmed binding is displaced at capacity).
- Query surface for Phase 4: identity_store_lookup(address) -> const
  identity_pin_t* {ed25519_pub, x25519_pub, pinned_at_ms,
  last_confirmed_ms}, plus identity_store_count.
- Counters are C-API-only this phase (getStatus wiring would touch the RPC
  contract; deferred as the brief allowed).

Impersonation test result (the campaign payoff, host + gosim): a keyed
insider attesting a victim's address under its own key produces an
internally valid frame (its sig verifies against its own embedded key);
the TOFU pin REFUSES the re-bind, the counter increments, and lookup
still returns the victim's original keys. Non-vacuous: the genuine
binding pins and verifies first in every variant of the test.

## Tests

Host (test/run_all_tests.sh: 101/101 suites pass; 3 new suites, 25 new tests):
- test_identity_attestation.c (updated): 158-byte layout, offsets 144/152,
  round trip incl. new fields, exact-length, signed-msg unchanged +
  auth_hmac/seq exclusion.
- test_ident_relay_auth.c (new): MAC round trip on/off the wire; per-field
  tamper (src, x25519, ed25519, sig, seq, mac) fails with non-vacuous
  controls; header fields excluded; wrong network key fails and re-sign
  under it recovers.
- test_identity_store.c (new): first pin, idempotent refresh, conflict
  rejection with original surviving (Ed and X25519-only mismatch),
  conflict does not refresh LRU, LRU eviction order at capacity, delivered
  pin, bad-Ed-sig not pinned + counted, self ignored, end-to-end
  impersonation via the delivery path.
- test_ident_relay_gate.c (new): mirrors handle_identity_attestation with
  the real components (test_flooded_ack.c pattern): good frame delivered +
  relayed byte-identical except hop_limit (explicit 158-byte compare);
  bad MAC dropped without touching the replay window (good-MAC control
  first); rewritten-packet_id replay killed by the seq window;
  MAC-valid-sig-invalid RELAYS but never pins (residual, pinned as a
  test); own echo neither relays nor pins; budget-denied/hop-exhausted
  still deliver.

gosim (go build + go test -count=1 ./... green):
- identity_attestation_test.go: 5-node line (A..E, 100-unit spacing,
  150-unit range). A's attestation crosses 4 MAC-gated relay hops through
  the real firmware C path and pins at B, C, D and E with A's exact key
  (multi-hop relay + pin proven at the far edge). Then E, a keyed insider,
  attests A's ADDRESS under its own key: C and D emit identity_conflict,
  keep A's key, reject E's; A ignores it as self; the forged binding is
  never pinned anywhere; re-pins stay idempotent (no second pin event).
- Bridge parity: _handle_identity_attestation mirrors the firmware order;
  origination mirrors send_identity_attestation; relay goes through
  bridge_flood_relay (the one shared engine). Every node gets a real
  Ed25519 keypair + pin store at join; initial scenario nodes now run
  bridge_handle_node_join_ext like dynamic joins (they previously skipped
  ext init entirely, a pre-existing gap).

## CI evidence

- Host: 101/101 suites (test/run_all_tests.sh, ASan build).
- Board: make ci-quality-board-build green (heltec-v3, full firmware link
  with the new handler + identity_store in the identity component).
- clang-format v14 --strict: PASS (378 files) in
  bramble/runner-full:22.04-go126.
- cppcheck (same image, CI flags over main + components): clean.
- check-rpc-contract: OK (53 methods; contract untouched).
- gosim: go build ./... && go test -count=1 ./... green.

## Residuals / notes

- RAM-only pins: reset on reboot, TOFU re-establishes. NVS persistence of
  pins deliberately out of scope (noted in identity_store.h).
- Relay-of-sig-invalid: a keyed insider's MAC-valid garbage-sig frame
  floods (bounded by the airtime budget); receivers reject at the Ed25519
  check and count it (sig_failures). Accepted and documented in
  routing_auth.h + the handler comment + pinned by a test.
- First-seen-wins means a node that heard the IMPERSONATOR first pins the
  wrong binding until reboot; the genuine node's later attestations then
  show as conflicts (the detectable signal). Inherent to TOFU without an
  out-of-band root of trust; Phase 4 material.
- TOFU pins are forgeable-in-principle under the unprovisioned public-PSK
  fallback network key only at the RELAY gate, not at the claim: the Ed
  sig still binds keys to the address claim regardless of network key
  provisioning.
- gosim does not model the control replay window (precedented; noted in
  the bridge comment): the replay gate is host-tested
  (test_ident_relay_gate.c) and live in firmware.
- gosim counters ed8 events carry only a 4-byte key prefix; sufficient for
  scenario assertions, not a full-key export.
