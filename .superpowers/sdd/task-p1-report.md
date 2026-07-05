# Phase 1 report: Ed25519 identity keypair + NVS + pubkey_hash divergence fix

Status: DONE
Branch: feat/ed25519-identity (worktree /home/user/src/worktrees/ed25519-identity)
Base: 633137ef (Phase 0 Ed25519 primitive)

## Commits

1. 09612dec fix(crypto): derive pubkey_hash as independent SHA256[4:8] on device
2. 124ffb03 feat(crypto): give every node identity a persistent Ed25519 keypair
3. 196686bd feat(identity): persist Ed25519 keys in NVS with old-blob migration

## Identity struct change (components/crypto/include/crypto.h)

bramble_identity_t gains, between the X25519 keys and address/pubkey_hash:

- uint8_t ed25519_public_key[BRAMBLE_ED25519_PUBKEY_SIZE]  (32)
- uint8_t ed25519_private_key[BRAMBLE_ED25519_SECKEY_SIZE] (64, libsodium
  layout seed || public, as pinned by the Phase 0 primitive)

bramble_identity_t is not a wire struct; no wire/packet/RPC change. The
beacon pubkey_hash field semantics on the wire are unchanged (still a
uint32 written by packet.c put_be32); only its derivation was fixed.

## crypto_generate_identity wiring (both backends, identical behavior)

- Host (components/crypto/crypto_host.c, crypto_generate_identity): after
  the X25519 raw keys are extracted, crypto_ed25519_keypair(id->ed25519_
  public_key, id->ed25519_private_key) is part of the same && chain;
  address/pubkey_hash are only set and ret only becomes 0 if it succeeds.
- Device (components/crypto/crypto_esp.c, crypto_generate_identity): the
  mbedtls ecp_mul + public-key write collapse into an ok flag; then
  crypto_ed25519_keypair runs and clears ok on failure; address and
  pubkey_hash are only computed when ok. Fail closed: an entropy-gate-shut
  (SEC-L1) or keygen failure propagates as -1, never a partial identity.
- Address is STILL X25519-derived: id->address = crypto_derive_address(
  id->public_key) on both backends, untouched semantics (SHA256(x25519_pub)
  [0:4]). The Ed25519 rebind is deliberately left to Phase 3.

## pubkey_hash divergence fix

- Bug: device crypto_esp.c:37 returned crypto_derive_address(pub) =
  SHA256(pub)[0:4] (identical to the address, zero extra entropy), host
  crypto_host.c returned SHA256(pub)[4:8]. identity_check_collision was a
  no-op / wrong on device.
- Fix: device crypto_derive_pubkey_hash (crypto_esp.c, now lines 37-49)
  computes SHA256(pub)[4:8] exactly like the host version (crypto_host.c
  lines 25-30), each as an independent implementation of the same spec.
  Both backends now: address = SHA256[0:4], pubkey_hash = SHA256[4:8],
  independent slices.
- Spec pinning: since only the host backend runs in the host suite, the
  exact bytes are pinned in test/test_identity.c for public_key =
  00 01 ... 1f: address 0x630DCD29, pubkey_hash 0x66C43366 (SHA256 =
  630dcd2966c43366...). Any backend drifting from the [0:4]/[4:8] spec
  fails this test.

## NVS persistence + migration (components/identity/identity.c)

- identity_save/identity_load are now a shared platform-independent core
  over a tiny blob-store API (id_store_read exact-length + fail-closed,
  id_store_write): device backend = NVS blobs in NVS_NS_IDENTITY with
  nvs_set_blob + nvs_commit both checked; host backend = in-memory store
  (identity_host_store_reset() resets it, declared host-only in
  identity.h) so the unit tests exercise the real shared logic rather
  than a stub.
- Blob keys: existing "priv"/"pub" (X25519, 32 each) plus new "ed_pub"
  (32) and "ed_priv" (64).
- MIGRATION CHOICE: keep the existing X25519 identity and generate +
  persist just the missing Ed25519 keypair. Rationale: it is one branch
  in identity_load, and since the address is X25519-derived this phase,
  an upgraded node keeps its address (regeneration was allowed but is
  strictly more disruptive for zero code savings). Fail closed: if the
  Ed25519 keygen or the persist of either blob fails, identity_load
  returns -1 and main.c falls back to identity_generate_and_save exactly
  as before.
- Fresh-flash behavior unchanged: no "priv"/"pub" blobs -> load fails ->
  caller generates and saves a full (now X25519+Ed25519) identity.

## Tests (test/test_identity.c, 10 tests, all green)

- test_pubkey_hash_pinned_to_independent_slice: pins address=SHA256[0:4]
  = 0x630DCD29 and pubkey_hash=SHA256[4:8] = 0x66C43366 for a fixed key,
  and asserts they differ (pins the device spec from the host suite).
- test_generated_identity_hash_distinct_from_address: generated identity
  has pubkey_hash == derive(pub) and != address.
- test_generated_identity_has_working_ed25519_keypair: sign with
  id.ed25519_private_key, verify with id.ed25519_public_key (Phase 0
  primitive), reject a wrong message, check sk layout seed||pub, and
  assert the address is derived from the X25519 key and NOT from the
  Ed25519 key.
- test_two_identities_have_distinct_ed25519_keys.
- test_identity_load_fails_on_empty_store.
- test_identity_save_load_roundtrips_all_keys: all four key blobs +
  address/pubkey_hash round-trip; reloaded Ed keypair still signs.
- test_identity_migration_from_x25519_only_store: store seeded with only
  "priv"/"pub"; load succeeds, X25519 keys and address preserved, a
  working Ed25519 keypair exists, and a SECOND load returns the same Ed
  keys (proves persistence, not per-boot regeneration).
- Plus the 3 pre-existing collision-check tests.

TDD: cycle-2 and cycle-3 tests were written first and observed red
(missing struct members; missing store API / stub load). The cycle-1
pinning test cannot go red on host because the host backend was already
[4:8]; it exists to pin the device implementation to the shared spec.

## CI

- Host suite: 97/97 suites pass (bash test/run_all_tests.sh).
- Board build heltec-v3: green (idf.py via make ci-quality-board-build,
  ESP-IDF 5.4.1; binary 0x161000, 54% partition free); re-run against
  final HEAD after the formatting amend.
- clang-format v14 (runner image, --strict): PASS, 371 files. Note: the
  check must be run with the main repo's .git mounted, since this is a
  linked worktree.
- cppcheck (runner image): clean, exit 0.
- check-rpc-contract: OK, 53 methods match.
- gosim: go build + go test -count=1 pass.

## Concerns / notes

- Old nodes migrate on first boot after OTA: same address, new Ed25519
  keys persisted. Nothing consumes ed25519_* fields yet outside tests;
  Phase 3 (attestation + address rebind) is the consumer.
- identity_load still writes into the caller's struct before it can fail
  (matches pre-existing behavior); callers already treat -1 as "no
  identity" and regenerate.
- The worktree has a pre-existing uncommitted .superpowers/sdd/progress.md
  edit that is not mine; left untouched.
