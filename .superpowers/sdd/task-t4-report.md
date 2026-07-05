# Task 4 report: docs + competitive delta + dead-code cleanup (campaign close)

Status: COMPLETE. Full CI green.

## Commits (on feat/mandatory-provisioning)

1. `chore(webapp): remove dead client-side network-key gen helpers` (5a34021e)
2. `docs: reflect mandatory provisioning (no public-PSK fallback, inert unprovisioned node)` (2be5d18c)

(This report + ledger land in a third `docs(sdd)` commit, matching prior-task convention.)

## Part A: docs

Verified every claim against the current code before writing (network_key.c,
routing_auth.c, mesh_task.c send-gates, rpc_methods.c, public_channel.c).

### Residual documented as CLOSED

The residual previously tracked as "SEC-H1/H2/NEW-SEC-4/NEW-SEC-8, everything
forgeable until real per-fleet key provisioning lands" is now documented as
CLOSED BY CONSTRUCTION for the KEYLESS-outsider / forgeable-until-provisioned
half. Evidence cited in the docs:

- `network_key_get()` fails closed (returns -1, writes nothing; no PSK fallback).
- `network_key_mac()` emits an all-zero sentinel and returns nonzero unprovisioned.
- Every verify (`rerr_verify`, `ack_verify`, `receipt_verify`, `data_auth_verify`,
  `ident_relay_verify`) rejects BEFORE the constant-time compare, so a received
  all-zero MAC can never match the emitted all-zero sentinel.
- Beacon: send side zeroes `s_beacon_key` and skips origination; RX drops before
  `beacon_verify_hmac`; `mesh_rederive_beacon_key` explicitly refuses to derive
  from `BRAMBLE_PUBLIC_CHANNEL_PSK`.
- All authenticated send paths gated on `network_key_is_provisioned()` /
  `data_auth_sign() != 0` (beacon, RREP, RERR, ACK, delivery receipt, identity
  attestation, DATA x4).
- Provisioning is mandatory and NVS-persisted: on-device `bramble.generateNetworkKey`
  founder key, `bramble.setNetworkKey` paste/join, or NVS at boot.

### Boundaries stated (NOT overclaimed)

- Keyed-INSIDER control-MAC forgery residual is UNCHANGED. Narrowed by per-node
  Ed25519 identity (prior campaign), NOT by this provisioning work. Section 5's
  insider-forgery residual left intact.
- The opt-in PUBLIC BROADCAST CHANNEL (`public_channel.c`,
  `BRAMBLE_PUBLIC_CHANNEL_PSK`) is a deliberate unauthenticated-to-everyone
  feature with a public key and is NOT a control-plane default. Confirmed the PSK
  is referenced only in the public-channel path now, never in the control plane.
  Added an explicit boundary callout to the spec's Public Channel section.
- NEW-SEC-4 bootstrap-quorum race is mitigated, not closed (unchanged).

### Files edited

- `docs/SECURITY-MODEL.md`: active-RF-injector class, beacon section (retitled),
  control-plane section (rewrote the "This does not close" paragraph into
  "closed by construction" + explicit "what this does NOT close"), section 4 gap
  bullets (control-plane auth, replay, mailbox flush), DATA `auth_hmac` note.
- `docs/bramble-protocol-spec.md`: wire-v2 beacon note (471), section 4.25 items
  8 and 9, and a new boundary callout in the Public Channel section.
- `README.md`: "Authenticated traffic (wire v4), no insecure bootstrap" bullet.
- `main/mesh_task.c`: corrected one stale comment describing removed PSK fallback.

### Competitive delta

Added to `docs/COMPARISON.md`: corrected all four stale "unprovisioned network
falls back to a public PSK" claims (table rows, summary point 1, strengths point
5), and added a dedicated strengths point 8, "No insecure bootstrap, no public
default key (greenfield property)": Bramble refuses to emit or accept an
authenticated frame until it holds a real per-fleet key, a structural property
Meshtastic cannot match because it ships a well-known public default channel/PSK
its installed base depends on. Stated the honest boundaries in-line: this is
control-plane authentication (not confidentiality of the opt-in public channel),
and insider forgery remains (addressed by per-node identity, not this campaign).

## Part B: dead-code cleanup

Removed from `webapp/src/utils/networkKeyShare.ts`: `generateNetworkKeyHex`
(no callers), `networkKeyFingerprint` (only its own test), and the `hexToBytes`
helper that only fed the fingerprint. Removed the orphaned fingerprint test from
`webapp/src/utils/__tests__/networkKeyShare.test.ts`. Kept the still-used share/QR
codec (`encodeNetworkKeyShare` / `parseNetworkKeyShare`, used by NetworkKeySection
and QRScanModal).

## CI

- Host tests: 101/101 suites passed.
- clang-format v14.0.0 (379 files clean) + cppcheck: PASS (docker runner-full).
- check-rpc-contract: PASS.
- gosim `go test -count=1 ./...`: PASS.
- webapp typecheck + 272 unit tests + production build: PASS.
- Board build (`make ci-quality-board-build`, heltec-v3): PASS.

## Concerns

None substantive. Only firmware change is a one-line comment; no behavior change.
