# Task P4 report: address rebind to Ed25519 + identity-gated ops + docs (campaign close)

Status: DONE. Branch feat/ed25519-identity, base 7ddd6c48 (P3 head).

## Commits

1. f2435a3a feat(crypto)!: rebind the node address to the Ed25519 identity key
2. 3df6c0e2 feat(identity): reject attestations whose src_addr is not their key's address
3. c9b9c7f1 feat(gosim): derive sim node addresses from real Ed25519 identity keys
4. b7bc3062 feat(mesh): gate the timesync corroboration quorum on pinned identity
5. ce6ef6ad feat(mesh): enforce DM key continuity against pinned identities
6. c39f316c feat(rpc): identity pin diagnostics in bramble.getStatus
7. d5f648db docs: per-node identity security model, attestation wire spec, honest competitive delta
8. (this report)

## Part A: the rebind

Both backends (components/crypto/crypto_esp.c, crypto_host.c) and
identity_load (components/identity/identity.c) now derive address and
pubkey_hash from ed25519_public_key. The derivation FUNCTIONS are
unchanged (SHA256[0:4] / [4:8]); only the input key changed, so
test_identity.c keeps the pinned 0x630DCD29 / 0x66C43366 constants for
input 00..1f as the function spec, and the identity-level tests pin the
new input: address == derive(ed_pub) and != derive(x25519_pub), for
generate, load, and the migration path.

### Caller inventory (every crypto_derive_address caller / addr<->X25519 coupling) and how each reconciled

1. components/crypto/crypto_esp.c + crypto_host.c
   (crypto_generate_identity): input flipped to ed25519_public_key.
2. components/identity/identity.c (identity_load): same flip; migration
   of a pre-Phase-1 X25519-only store now produces a NEW address (the
   flag day, see below). Address is DERIVED on load, never stored;
   confirmed by reading identity_save/identity_load (only the four key
   blobs are persisted).
3. components/dm_session/dm_session.c (dm_verify_init, dm_verify_resp):
   THE coupling. Both enforced derive_address(long_term_pubkey) ==
   src_addr, which cannot hold post-rebind (long_term_pubkey is X25519).
   Replaced by the Phase 4 pin-continuity parameter
   (pinned_peer_x25519_or_null; mismatch = DM_VERIFY_ERR_PIN_MISMATCH).
   RESP src_addr tampering stays rejected via the K_confirm transcript
   (test updated to pin that specific mechanism). INIT first contact
   with no pin is TOFU-grade: stated residual, closes when the peer's
   attestation is pinned. dm_session.h documents all of it.
4. test/test_dm_session.c, test/test_ke_envelope.c: call sites +
   spoof-test rewrites per item 3.
5. test/test_identity.c, test/test_crypto.c: test_crypto uses raw byte
   inputs (function-level, input-agnostic): no change needed.
6. test/test_identity_store.c + test/test_ident_relay_gate.c: test
   attestation builders now claim derived addresses (the honest case);
   explicit claim_addr override models the attacker.
7. simulator (gosim): sim nodes now hold a persistent Ed25519 identity
   on sim_node_t, created once at node_array_add (NVS analog,
   deterministic seed from node id for reproducibility), and node->addr
   = crypto_derive_address(ident_ed_pub). Attestation origination signs
   with those keys. Verified by TestSimNodeAddressesDeriveFromIdentityKeys,
   which recomputes SHA256[0:4] independently in Go and compares. The
   radio/budget unit-test harness (radio_harness.go addNode) pins
   caller-chosen addresses back after add: unit scaffold only, those
   tests never exercise attestation delivery; every full-sim path
   (scenario load, node join, add_node) keeps the derived address.
8. main/rpc_methods.c getStatus/getIdentity and webapp: echo the address
   value only, never the derivation; no change needed beyond docs.
9. KEY_EXCHANGE frame itself: unchanged on the wire (long_term_pubkey
   remains the X25519 key; the Ed25519 key never rides KE frames).
10. New crypto primitive: crypto_ed25519_keypair_from_seed (both
    backends; RFC 8032 TEST 1 seed->pub pinned in test_ed25519.c),
    needed for gosim determinism; production keygen wraps it.

### The security payoff (addr == derive(ed_pub) in the store)

identity_store_handle_attestation order: self short-circuit ->
crypto_derive_address(att->ed25519_pub) == att->src_addr (one SHA256,
BEFORE the expensive Ed25519 verify) -> Ed25519 verify -> TOFU pin.
Mismatch = new IDENTITY_PIN_ADDR_MISMATCH result + addr_mismatches
counter + mesh_task WARN. Tests are non-vacuous: the SAME code path
first pins an honest claim (control), then rejects a validly-signed
claim of a never-pinned victim address ON FIRST CONTACT, and the
impersonation-via-delivery test proves the victim's existing pin
survives untouched. The TOFU CONFLICT path remains reachable and tested
for what the address does NOT bind: X25519 rotation under the same Ed
key (the DM red flag) and the 2^32-work address-colliding-Ed-key case.
gosim proves it end to end through the real relay path (5-node line;
forged claim rejected as identity_addr_mismatch at every receiver, no
conflict ever reached; separate rotation scenario keeps conflict
coverage).

### Flag-day statement (fleet upgrade)

The address is derived, not stored (confirmed). An upgrading node's
identity_load keeps its X25519 keys, generates+persists Ed25519 keys if
absent (Phase 1 migration), and derives its address from the Ed key: so
EVERY pre-rebind node comes up with a NEW address exactly once,
post-upgrade. Consequences: peers' identity pins are RAM-only and
re-establish by TOFU (stale pins for old addresses age out via LRU or
reboot); routes/neighbor entries for old addresses expire normally;
msg_store conversation history keyed by old peer addresses is orphaned
(display-only loss, accepted pre-alpha); no old/new shim exists, same
policy as the wire-version flag days (spec sections 4.25-4.27). A mixed
fleet is safe but pointless: old and new nodes interoperate at the
packet level (wire format unchanged this phase), but old nodes never
attest, so on a new node's mesh they are simply never pinned (and once
pins exist they drop out of the new nodes' timesync quorum). Upgrade
the whole fleet together.

## Part B: identity-gated ops (graceful, never brick)

### Timesync quorum gate

Chosen semantic (identity_store_quorum_eligible, documented in
identity_store.h, wired at mesh_task's single timesync_handle_sync call
site):

    eligible = established AND (pinned OR store holds ZERO pins)

Tenure (ws 1.3c neighbor_is_established) always required, never
relaxed. Once ANY pin exists, only pinned peers corroborate: combined
with the rebind, fabricated bare addresses are unpinnable (no deriving
key), so the NEW-SEC-4 fabricated-address corroboration path is gone.
Zero pins = tenure-only fallback: a fresh mesh (or any node right after
boot; pins are RAM-only) converges exactly as before. Both the gated
path and the no-pins fallback are unit-tested
(test_identity_store.c: test_quorum_no_pins_falls_back_to_tenure_only,
test_quorum_with_pins_requires_pinned_identity). Accepted transient:
between the first pin and the rest of the neighbors' attestations,
unpinned established neighbors are excluded (bounded by the boot-hook
attestation + 15-min cadence); post-commit timesync paths unaffected.
Unpinned peers lose ONLY quorum membership: still neighbors, relays,
DM peers.

### DM key continuity

dm_verify_init/dm_verify_resp take pinned_peer_x25519_or_null; pinned
and different long-term key = DM_VERIFY_ERR_PIN_MISMATCH, refused with
a distinct "DM KEY CONTINUITY ... REFUSED" WARN in mesh_task. The pin
is SNAPSHOTTED into the handshake work item by handle_ke_envelope on
the mesh task (sole mutator of s_identity_pins), so the handshake
worker never reads the pin store cross-thread. No pin = today's
TOFU-grade behavior, unchanged (residual: first-contact window until
the attestation is pinned). Unit tests: matching pin accepted
(control), mismatched pin refused with the distinct code, no-pin
accepted; both INIT and RESP sides.

### Diagnostics

getStatus gains additive fields identity_pins, identity_conflicts,
identity_sig_failures, identity_addr_mismatches via new
mesh_get_identity_pin_stats (lock-free word reads, diagnostics-grade),
stubbed for host rpc builds, asserted in test_rpc_methods_firmware.c,
mirrored in api/openapi.yaml StatusResponse. check-rpc-contract green
(53 methods, unchanged set).

## Part C: docs + competitive delta

- docs/SECURITY-MODEL.md: Identity generation rewritten (dual keypair,
  Ed-derived address); new Current Protections section for the whole
  campaign with residuals stated plainly (free-to-mint identities, NO
  Sybil-scarcity claim, RAM-only pins, TOFU DM first contact,
  plaintext-NVS keys, budget-bounded keyed garbage floods, no
  revocation); NEW-SEC-4 residual rewritten to the shipped reality
  (still NOT closed; the bar is now "mint, attest, pin and sustain N
  real identities"; trust anchor deferred); section 4 staging bullet
  updated.
- docs/bramble-protocol-spec.md: new section 4.28 (158-byte frame
  layout, 84-byte canonical signed bytes, both authenticators, receiver
  order, 144->158 growth, ~0.02-0.05%/node duty at 15 min); packet-type
  table + firmware-reality note gain 0x15; section 5.1 pseudocode now
  matches shipped code (dual keypair, Ed-derived address, NVS keys,
  derived-not-stored) plus the flag-day paragraph; Sybil/impersonation
  rows updated.
- docs/COMPARISON.md: node-identity row rewritten; DM row notes key
  continuity; new "Per-node identity delta" subsection: narrows the
  MeshCore gap and exceeds it on address-key binding; Meshtastic has
  nothing comparable; explicitly no trust anchor and no Sybil-scarcity
  claim.

## CI (all green, run on the final tree)

- Host: bash test/run_all_tests.sh: 101/101 suites.
- Board: make ci-quality-board-build (ESP-IDF, heltec-v3): Done.
- clang-format v14 (runner image, --strict): PASS, 378 files.
- cppcheck (runner image, CI flags): exit 0.
- check-rpc-contract: OK, 53 methods.
- gosim: go build + go vet + go test -count=1 ./...: ok (includes the
  new addr-mismatch, rotation-conflict, and address-derivation tests).

## Concerns / carry-forward

- gosim's confirmed-delivery load scenario was re-tuned 2 -> 3 msgs/min
  (derived addresses reshuffled the deterministic collision pattern and
  the old run confirmed every delivered message; the test's own comment
  prescribes re-tuning). Worth remembering: that test is sensitive to
  address-dependent determinism.
- Accepted quorum transient (first-pin window) documented above and in
  identity_store.h; if field behavior shows timesync flapping around
  boot, a grace-window variant is the fallback design.
- KE first contact (no pin) remains TOFU-grade and lost the pre-rebind
  derive-address binding by necessity; the outer-envelope src
  consistency check and the pin gate are the compensating controls.
  Possible future hardening: use the pinned X25519 as peer_id_pub to
  make first INIT to a pinned peer an authenticated (rekey-style) INIT.
- Identity keys still plaintext-NVS; pins still RAM-only; no
  revocation; no trust anchor (Phase 5, deferred, owner decision).
