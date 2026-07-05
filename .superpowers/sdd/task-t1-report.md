# Task 1 report: network_key fail-closed core (remove PSK fallback, add generate + NVS load)

Status: DONE. Branch feat/mandatory-provisioning, base main a01a2f68.

## Commits

- `c4f142d0` feat(network_key): remove public-PSK fallback, fail closed when unprovisioned
- `0480637a` feat(network_key): add entropy-gated key generation and NVS persistence

(Two logical pieces: the security core, then the generate/NVS machinery. Each
commit builds and its host unit tests pass.)

## The network_key API after the change

```c
void network_key_set_provisioned(const uint8_t key[32]); // in-mem + persists to store
void network_key_clear(void);                            // in-mem only, does NOT erase NVS
int  network_key_get(uint8_t key_out[32]);               // 0 iff provisioned; else nonzero, writes NOTHING
int  network_key_is_provisioned(void);
int  network_key_generate_provision(uint8_t key_out[32]);// entropy-gated; fail-closed
int  network_key_load_from_nvs(void);                    // 0 if a key loaded, nonzero if none
int  network_key_mac(const char* label, const uint8_t* data, size_t len, uint8_t out[8]);
void network_key_fingerprint(uint8_t out[4]);
// host-only test hook: void network_key_host_store_reset(void);
```

### network_key_mac fail-closed contract (the key decision)

Chose the PREFERRED option: **changed the signature `void` -> `int`**.
- Provisioned: returns 0, writes HMAC(net_key, label||data)[0:8] to `out`.
- Unprovisioned: returns nonzero AND writes the all-zero 8-byte sentinel to
  `out` (never an HMAC over a fallback/zeroed key).

Why this keeps the tree compiling: every existing call site invokes
`network_key_mac(...)` as a statement and discards the value. Discarding a
non-void return is legal C and warns under neither `-Wall -Wextra` nor
`-Werror` (no `warn_unused_result` attribute was added). So the board build
still links with zero caller edits. Callers do NOT yet check the return; Task 2
hardens them (see call-site list below). I did not need the `_checked`
variant fallback.

### Other contract points

- `network_key_get`: unprovisioned returns -1 and writes nothing (verified
  non-vacuously: a 0x5A-prefilled `key_out` is unchanged after a failed get).
- `network_key_fingerprint`: unprovisioned writes the all-zero 4-byte
  sentinel (documented "no key provisioned").
- `network_key_generate_provision`: draws 32 bytes from `crypto_random`
  (entropy-gated SEC-L1 source, same as crypto_generate_identity; NOT raw
  esp_random). Scratch-buffer pattern: on entropy failure returns nonzero and
  provisions nothing, `key_out` untouched. On success: set_provisioned (which
  persists) + copy to key_out.
- NVS: device store = NVS_NS_NETKEY / NVS_KEY_NETKEY (mirrors identity.c);
  host store = in-memory shim so load_from_nvs round-trips in unit tests.
  Persist happens on BOTH set and generate paths (set_provisioned writes the
  store). `network_key_load_from_nvs` reads only (no re-write).
- `network_key_clear` is in-memory only and does NOT erase NVS (so the NVS
  round-trip test can generate -> clear -> load).

## PSK fully removed (grep confirmation)

- `grep -rn BRAMBLE_PUBLIC_CHANNEL_PSK components/network_key/` -> 0 matches.
- `grep -rn "bramble-netkey-fallback" .` (c/h) -> 0 matches (salt deleted).
- The PSK-derivation branch in network_key_get is gone.

Scoping note: `BRAMBLE_PUBLIC_CHANNEL_PSK` (crypto.h) is NOT deleted, because
it is the well-known key for the SEPARATE public broadcast-channel feature
(components/channel/public_channel.c, test_public_channel.c) which is an
intentional public channel, not a network-key fallback. Deleting the constant
would break that unrelated feature. The network key no longer references it.

## Fail-closed tests (test/test_network_key.c, 13/13 pass)

Unprovisioned: get returns nonzero + leaves key_out untouched (0x5A sentinel);
fingerprint = all-zero; mac returns nonzero + emits all-zero sentinel.
Provisioned: set->get round-trips; clear->get fails (NOT a fallback);
generate yields non-zero random material and enables a non-zero MAC; a second
generate after clear yields a DIFFERENT key; fingerprint stable/convergent;
domain separation + MAC stability; NVS round-trip restores key+fingerprint;
set-path persists; load-with-nothing-stored returns nonzero.

## CI state

- network_key unit test: 13/13 PASS.
- Full host suite (`bash test/run_all_tests.sh`): 94/101 suites pass. 7 fail
  BY DESIGN (they relied on the public-PSK fallback; see next section). NOT
  fixed here (no fallback re-introduced) - that is Task 2's harness job.
- Board build (`make ci-quality-board-build`, heltec-v3): COMPILES + LINKS
  clean. No caller edits were needed (int-return discarded by callers).
- clang-format v14 (runner): PASS (378 files). cppcheck: clean.
- check-rpc-contract: OK (53 methods; rpc_methods.c untouched).
- gosim (`go build ./... && go test -count=1 ./...`): GREEN. gosim never
  provisions and never exercises MAC-based rejection, so fail-closed does not
  break it.

## Expected breakage for Task 2 to fix (host suites)

These 7 suites fail because they run control-plane auth WHILE UNPROVISIONED and
depended on the fallback key to produce a data-dependent MAC. With fail-closed,
an unprovisioned MAC is the all-zero sentinel regardless of data, so a tampered
field is not detected AT THE MAC LAYER (the verify functions do not yet check
network_key_mac's return). Every failure is a tamper/forgery-rejection assert
(`Expected FALSE Was TRUE`):

- test_ack_auth: test_ack_verify_rejects_tampered_ack_packet_id, test_ack_seq_covered,
  test_receipt_verify_rejects_tampered_orig_packet_id, test_receipt_seq_covered
- test_data_auth: test_f1_zero_hmac_installs_no_route, test_f1_src_addr_tamper_breaks_mac,
  test_covered_header_fields_are_bound
- test_discovery: test_rrep_build_intermediate_tamper_fails_verify
- test_ident_relay_auth: test_tampered_{src_addr,x25519,ed25519,sig,seq}_fails
- test_rerr_auth: test_rerr_verify_rejects_tampered_broken_dest, test_rerr_reporter_addr_covered,
  test_rerr_seq_covered, test_rerr_verify_survives_reorigination
- test_rrep_auth: test_rrep_seq_covered_by_mac, test_rrep_verify_rejects_flipped_route_metric
- test_unicast_flood: test_flood_on_forged_src_never_relays

Task 2 harness fix: provision a shared test key in each suite's setUp (they
currently setUp with network_key_clear()), so happy-path and tamper tests run
against a real MAC. Some cases additionally need the verify functions to check
network_key_mac's return (see next).

## Exact call sites Task 2 must harden (check network_key_mac's int return)

`network_key_mac` is called at 12 sites; all currently discard the return.
Task 2 must make each refuse to originate/verify when it is nonzero:

- components/routing_auth/routing_auth.c:45,52 (bramble-rerr-v2 build/verify)
- components/routing_auth/routing_auth.c:77,84 (bramble-ack-v2 build/verify)
- components/routing_auth/routing_auth.c:109,116 (bramble-receipt-v2 build/verify)
- components/routing_auth/routing_auth.c:135,142 (bramble-data-v1 build/verify)
- components/routing_auth/routing_auth.c:165,172 (bramble-ident-relay-v1 build/verify)
- components/routing/discovery.c:141,148 (bramble-rrep-v2 build/verify)

Other network_key consumers Task 2 should reconcile:
- main/mesh_task.c `mesh_load_network_key()` (line ~5420) and rpc_methods.c
  `rpc_set_network_key` (line ~771) each hand-roll their own NVS read/write.
  The component now owns NVS (load_from_nvs + persist-on-set). Task 2 should
  switch boot to `network_key_load_from_nvs()` and drop the redundant NVS code
  (harmless duplicate today; boot double-writes the same value).
- main/mesh_task.c `mesh_rederive_beacon_key()` (line ~5448-5453) STILL derives
  a beacon HMAC key from BRAMBLE_PUBLIC_CHANNEL_PSK when unprovisioned. This is
  a SURVIVING network-key-adjacent fallback in the control plane; Task 2 must
  make the beacon path inert when unprovisioned (out of scope for Task 1, which
  is component-only).

## Concerns

- Interim security gap by design: while unprovisioned, network_key_mac returns
  the all-zero sentinel, so if a Task-2-unhardened caller ignores the return it
  would treat all-zero as a valid MAC (an all-zero forged MAC would verify).
  This is exactly why the 12 call sites above must check the return before
  merge. Not exploitable on a provisioned node; the branch must not merge to a
  shipping build until Task 2 hardens the callers and gates the control plane.
- The .superpowers/sdd/progress.md ledger was already modified in the worktree
  at session start (campaign setup, not by me); left untouched.
