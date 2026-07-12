# DM Forward Secrecy and SAS Verification: Design Spec

Status: DRAFT design spike. Nothing here is implemented. This document proposes
two additions to the direct-message (DM) subsystem and the exact mechanisms
they would use, tied to the code that exists on `main` today. It follows the
style of `docs/SECURITY-MODEL.md`: every claim about current behavior is tied
to a cited file, and every proposed property is stated with its residual.

Two README gaps motivate this:

1. "No forward secrecy yet" for DMs.
2. "the SAS-comparison UX does not ship yet".

Both must compose with the existing DM crypto, not replace it. The existing
crypto is sophisticated and correct for what it does; this design builds a
ratchet on top of the session it already establishes, and a UX on top of the
SAS it already derives.

Before any of this ships, the key schedule and the SAS redefinition in
Part B need external cryptographic review (flagged inline and collected in
"Residuals and review gates").

---

## 0. What exists today (verified against `main`)

The DM session layer (`components/dm_session/dm_session.{c,h}`,
`main/mesh_task.c`):

- **Handshake.** A role-symmetric quad-DH X25519 exchange (X3DH-like), two
  messages `INIT`/`RESP` carried as `bramble_key_exchange_t`
  (`components/packet/include/packet.h`) inside `PKT_TYPE_DATA` envelopes with
  `app_type == APP_TYPE_KE`, under the channel key (`send_ke_envelope` in
  `main/mesh_task.c`). `dm_compute_ikm` builds a 128-byte IKM =
  `DH1(eph,eph) || DH2(id,id) || sorted{DH3(my_eph,peer_id), DH4(my_id,peer_eph)}`.
  `dm_session_key_from_ikm` derives the 32-byte session key via
  `HKDF-SHA256(salt="bramble-dm-v2", ikm, info = addr_lo||addr_hi||ke_epoch)`.
  Key-confirmation HMAC tags (`K_ke_init`, `K_confirm`) authenticate the
  transcript. Low-order X25519 points are rejected (`crypto_x25519_check_shared`).

- **SAS.** `dm_derive_sas(session_ikm[128], sas_out[8])` renders a 7-digit
  decimal short authentication string from `HKDF(salt="bramble-sas", ikm)`.
  **It commits to the full 128-byte session IKM, which includes both
  ephemerals (DH1) and both identities (DH2, DH3, DH4).** This matters in
  Part B: today the SAS is session-scoped, not identity-scoped.

- **Session state.** `dm_session_t` holds one static 32-byte `session_key`,
  `peer_id_pub`, a `ke_epoch`, a `msg_count`, a `verified` flag, and
  `last_active_ms`. The `verified` flag exists but is **never set** anywhere:
  SAS confirmation is not wired (`process_ke_init`/`process_ke_resp` set
  `verified = 0` with the comment "SAS confirmation is a separate UX step,
  not wired here"). 32 slots, RAM-only, LRU eviction with verified-active
  slots protected (`dm_alloc`).

- **Message encryption.** `send_dm_packet` encrypts each chat payload with
  AES-256-GCM under the **static** `session_key`, using the node-global 48-bit
  deterministic nonce counter (`nonce_counter_next`) as the 12-byte GCM nonce,
  AAD = masked header + `src_addr` (`bramble_build_aead_aad`). The session
  plaintext is raw chat bytes with no inner framing (`handle_data`: "Session
  payloads are always chat in this wiring").

- **`ke_epoch`.** Present in the key schedule (`dm_build_info` mixes it into
  the HKDF info) and on the wire (the `key_id` byte of `bramble_key_exchange_t`),
  but effectively always `0`: `initiate_dm_handshake` builds INIT with
  `ke_epoch = 0` and there is no proactive-rekey trigger wired (the comment at
  `initiate_dm_handshake` states proactive rekey is "out of this task's wiring
  scope"). A rekey code path exists in `dm_build_init` (the `peer_id_pub`
  non-null B1 construction) but nothing drives it.

- **Desync heal (commit `8ab55838`, PR #138).** When `handle_data` cannot
  decrypt a session DM (peer rebooted and lost its half, or key drift), it
  calls `maybe_trigger_dm_rehandshake`: a rate-limited (15 s/peer,
  neighbor-gated) re-INIT that tears down the stale session and re-establishes
  it. `process_ke_init` has a matching one-sided recovery: a strict-tag INIT
  from an attestation-pinned peer that fails verification is re-accepted as
  first contact after tearing the stale session down. **Any forward-secrecy
  design must reuse this exact failure path as its own desync recovery.**

- **Identity binding.** The node address derives from the Ed25519 key. DM
  X25519 keys are pinned via attestation-verified TOFU
  (`components/identity/identity_store.c`); a pinned peer whose DM
  `long_term_pubkey` changes fails with `DM_VERIFY_ERR_PIN_MISMATCH`
  (`dm_verify_init`/`dm_verify_resp`). A disagreeing pin tears down the stale
  TOFU session (`dm_pin_disagrees`, `dm_session_teardown`).

### Available primitives (`components/crypto/include/crypto.h`)

X25519 DH (with a contributory low-order check), Ed25519 sign/verify,
HKDF-SHA256, HMAC-SHA256 (and a trunc-4 helper), AES-256-GCM, SHA-256,
`crypto_random`. **This design uses only these.** It introduces no new
primitive. Every ratchet step is an HKDF-SHA256 call; every DH ratchet step is
an X25519 call. If review concludes a construction below needs a primitive not
in this list, that is a blocking finding.

### The wire budget (quantified)

DM DATA frame (`send_dm_packet`, `BRAMBLE_DATA_*` offsets in `packet.h`):

```
header(12) + src_addr(4) + prev_hop(4) + auth_hmac(8) + nonce(12) + ciphertext(N) + tag(16)
= 56 + N bytes total
```

`send_dm_packet` caps total at 255, and `mesh_send_dm` rejects any payload over
`FRAG_MAX_PLAINTEXT` (154 bytes) rather than fragmenting under a session key.
So the usable session plaintext is at most 154 bytes; the buffer cap is not the
binding constraint, airtime is. On the long-range default radio profile
(SF10/125 kHz) a short chat message is a few tens of bytes and time-on-air is
dominated by payload length. **This is the number that kills a per-message DH
public key:** adding 32 bytes to a 30-byte chat message roughly doubles its
airtime. Adding 3 bytes does not. Every overhead claim below is measured
against this 154-byte usable plaintext and the reality that real payloads are
far smaller.

---

# Part A: Forward secrecy for DMs

## A.1 Goals and threat model

**What FS must protect.** Past-message confidentiality after a *later*
compromise. Concretely: an attacker who, at time T, extracts a device's live
DM session state (the `session_key` in RAM, or a plaintext-NVS dump per the
device-thief adversary in `SECURITY-MODEL.md` section 1) must not thereby be
able to decrypt DM ciphertext they recorded off the air *before* T. Today they
can: the `session_key` is static for the life of the session, so one key
recovers every message the session ever carried, past and future
(`SECURITY-MODEL.md` asset 4 states plainly "There is no forward secrecy").

**Post-compromise (future) secrecy, stated separately.** After a compromise at
T, can the session *recover* so that messages after some T' > T are secret
again? This requires injecting fresh DH entropy the attacker did not capture.
It is a weaker, coarser goal here than per-message FS, and the recommendation
buys it only at bounded intervals, not per message. Called out explicitly
because it drives the option tradeoffs.

**What FS explicitly does NOT protect (honest residuals, unchanged by this
work):**

- **The network-key insider.** Every DM DATA frame still carries the
  network-key `auth_hmac` and rides the shared flood. An insider sees the
  frames, their timing, sizes, and `src_addr`. FS protects *content*, not
  metadata, and does nothing about a member who legitimately forwards your
  ciphertext. This is the shared-symmetric-key residual already accepted in
  `SECURITY-MODEL.md` section 5.
- **The unauthenticated first-contact TOFU window.** FS says nothing about
  *who* you established with. A MitM who substitutes keys during the
  first-contact handshake (before the peer's attestation is pinned) gets a
  forward-secret channel to the *attacker*. That is Part B's job (SAS), not
  Part A's. FS and authentication are orthogonal; do not let a "forward
  secret" claim launder into an "authenticated" one.
- **A compromise that captures the ephemeral private keys mid-handshake.**
  FS protects messages whose keys have already been ratcheted past and wiped.
  It cannot protect a message whose key material still lives in RAM at the
  moment of compromise. The guarantee is "past, already-advanced" not "all".
- **Concurrent live compromise.** If the attacker has code execution on the
  running device, current-and-future plaintext is theirs regardless
  (`SECURITY-MODEL.md` out-of-scope list). FS targets the *recorded ciphertext
  plus later key extraction* case specifically.

## A.2 The hard constraint

LoRa PHY: tiny MTU, low bandwidth, high loss, out-of-order and asynchronous
delivery, possibly long gaps between messages. Two consequences dominate the
design:

1. **Per-message overhead is expensive.** A Signal-style Double Ratchet puts a
   fresh 32-byte DH public key in every message header. Against a 154-byte
   ceiling and typical sub-40-byte payloads, that is a large tax on every
   message (section 0). Reject per-message DH.
2. **Skipped-message-key handling is the crux.** Under heavy loss and reorder,
   a receiver must derive and cache keys for messages it has not yet seen (or
   will never see). The cache must be *bounded* (a DoS surface otherwise: an
   attacker names a huge message index and forces unbounded key derivation and
   storage), and the bound-exceeded case must degrade gracefully into the
   existing desync-heal path rather than losing the conversation.

## A.3 Candidate designs

### Option A: symmetric KDF ratchet only

After the handshake, split the root into two **directional** symmetric chains,
labelled by the existing address ordering (`addr_lo`/`addr_hi`, already the
canonical order in `dm_build_info`): `CK[lo->hi]` and `CK[hi->lo]`. Each party
sends on its own directional chain and receives on the peer's. Per message:

```
mk_n   = HKDF(CK_n, "bramble-dm-mk")     # message key, used once, then wiped
CK_n+1 = HKDF(CK_n, "bramble-dm-ck")     # chain advances, CK_n wiped
```

- **Wire overhead:** a per-(session, direction) message index so the receiver
  can locate/skip. 2 bytes (see A.5). No DH on the wire, ever.
- **Under loss/reorder/gap:** cheap. Advancing the chain forward is a hash
  chain; the receiver derives skipped `mk`s up to the received index and
  caches a bounded number for out-of-order arrivals. A long gap just means a
  longer (bounded) forward walk.
- **FS:** yes, per message. Once `CK_n` and `mk_n` are wiped, message `n` is
  unrecoverable from later state.
- **Post-compromise secrecy:** **none.** A leaked `CK_n` compromises every
  message from `n` forward until the session is torn down and re-handshaked.
  There is no fresh DH to heal it.
- **Complexity:** low. Pure HKDF chains. No new handshake.
- **`ke_epoch` / desync-heal interaction:** clean. A desync-heal re-handshake
  resets the root and both chains; that re-handshake is the *only* PCS event.

### Option B: DH ratchet amortized per `ke_epoch`

Keep Option A's symmetric chains for per-message FS, and additionally run a
fresh mini-DH on a schedule (every N messages or every T minutes), advancing a
new root and incrementing `ke_epoch` (the field already exists in the schedule
and on the wire). The epoch bump reuses the INIT/RESP handshake machinery with
`peer_id_pub` known (the `dm_build_init` B1 rekey path that already exists but
is unwired).

- **Wire overhead:** a fresh 32-byte ephemeral **only on epoch-boundary
  messages**, not every message. Steady-state cost is Option A's 2 bytes.
- **Under loss/reorder/gap:** an epoch bump is a two-message handshake that can
  be lost; the sender retries, and until it completes both sides keep using the
  old epoch's chains (which still work). No message is stranded by a lost
  rekey.
- **FS:** per message (from the symmetric chain).
- **Post-compromise secrecy:** yes, at epoch granularity. A compromise heals
  once the next successful epoch bump injects DH entropy the attacker did not
  capture. Latency is bounded by N/T, not per message.
- **Complexity:** medium. Reuses existing handshake and `ke_epoch`; adds a
  rekey trigger and epoch-transition bookkeeping.
- **`ke_epoch` / desync-heal interaction:** excellent. The epoch bump *is* a
  handshake, so it flows through the same `process_ke_init`/`process_ke_resp`
  and the same desync-heal recovery. A failed rekey degrades to "stay on the
  current epoch", never to "session dead".

### Option C: loss-tolerant full Double Ratchet

The Signal Double Ratchet with a DH public key on every chain-switch message
and bounded skipped-key storage.

- **Wire overhead:** 32 bytes on (at least) every message that switches the
  sending chain, in practice frequent under the alternating traffic a
  conversation produces. Against the 154-byte ceiling this is the heaviest
  option.
- **FS + PCS:** the strongest, per message for both.
- **Under loss:** the classic skipped-key blowup, made worse by LoRa loss
  rates; bounded storage means dropping under sustained loss anyway.
- **Complexity:** high. The most moving parts, the most to get wrong, the most
  external-review surface.

## A.4 Recommendation

**Adopt Option B: Option A's symmetric directional chains for per-message
forward secrecy, plus a DH ratchet amortized onto the existing `ke_epoch`
mechanism for bounded-latency post-compromise recovery.**

This is a Double Ratchet with the DH ratchet decoupled from every message and
amortized onto an epoch schedule. It is built entirely from established
constructions (HKDF symmetric chains, X25519 DH ratchet); the only non-textbook
choice is *scheduling* the DH ratchet rather than running it on every chain
switch, which trades PCS latency (coarse, bounded by N/T) for steady-state wire
cost (2 bytes instead of 32 per message). That tradeoff is the whole point on
LoRa. **Flag for review:** the decoupled-DH-ratchet schedule is a known
variant, not a novel primitive, but the exact PCS latency bound and the
epoch-transition state machine need external cryptographic review before ship
(see "Residuals and review gates").

Rationale:

- Per-message FS (the README's stated gap and the higher-value property here)
  costs 2 bytes per message. That is the property worth paying for.
- PCS is real but coarse, and coarse is the right call on a medium where you
  cannot afford per-message DH. A device compromise heals within N messages or
  T minutes of the next epoch bump.
- It reuses `ke_epoch`, the INIT/RESP handshake, and the desync-heal path
  wholesale. Option C would bolt a second, parallel rekey mechanism onto a
  subsystem that already has one.

### Exact key schedule

Let `IKM` be the 128-byte handshake IKM (unchanged, `dm_compute_ikm`). Replace
the single `dm_session_key_from_ikm` output with a root and two chains:

```
RK_0        = HKDF(salt="bramble-dm-v2", IKM, info = addr_lo||addr_hi||epoch=0, L=32)
CK_0[lo->hi]= HKDF(salt=RK_0, ikm="", info="bramble-dm-chain-lohi", L=32)
CK_0[hi->lo]= HKDF(salt=RK_0, ikm="", info="bramble-dm-chain-hilo", L=32)
```

Per-message, on the sender's own directional chain at index n:

```
mk_n   = HKDF(salt=CK_n, ikm="", info="bramble-dm-mk"||n_be, L=32)
CK_n+1 = HKDF(salt=CK_n, ikm="", info="bramble-dm-ck", L=32)
```

`mk_n` is the AES-256-GCM key for that one message (nonce and AAD unchanged
from `send_dm_packet` today: node-global counter nonce, masked-header + src_addr
AAD). `mk_n` and `CK_n` are wiped immediately after use.

DH ratchet (epoch bump), when the schedule fires:

```
# initiator generates fresh eph', sends it in an INIT with key_id = epoch+1
dh      = X25519(my_eph'_priv, peer_eph'_pub)      # both sides fresh ephemerals
RK_e+1  = HKDF(salt=RK_e, ikm=dh, info = addr_lo||addr_hi||(epoch+1), L=32)
CK_0[..]= HKDF(salt=RK_e+1, ...) as above, chains reset, indices reset to 0
```

RK_0 keeps the current `salt="bramble-dm-v2"` derivation so the *first* epoch's
root is a pure function of today's IKM (migration continuity, A.6). Subsequent
roots chain from the previous root plus fresh DH, which is the Double Ratchet
root-KDF shape.

### Wire-format changes

The DM session plaintext today is raw chat bytes. Prepend a **3-byte ratchet
header inside the AEAD plaintext** (authenticated for free by the existing GCM
tag, no new AAD plumbing):

```
epoch    : 1 byte   (matches the low byte of ke_epoch, the handshake's key_id)
msg_index: 2 bytes  (per-session, per-direction, per-epoch, big-endian)
```

Cost: **3 bytes of the 154-byte usable payload, ~2%.** No change to
`bramble_key_exchange_t` for steady-state messages. The DH-ratchet INIT/RESP
reuse the existing `bramble_key_exchange_t` (it already carries a 32-byte
`ephemeral_pubkey` and `key_id`); the epoch bump is exactly a re-handshake with
a nonzero `key_id`, which the wire already supports.

Direction is not sent; the receiver infers it from `src_addr` versus its own
address against the `addr_lo`/`addr_hi` ordering. Epoch and index are inside
the ciphertext, so an attacker cannot flip them without failing the GCM tag.

### Skipped / out-of-order key policy (the crux, bounded)

Per session, per direction, per current epoch:

- Maintain the current receive chain `CK_r` at index `next`, plus a bounded
  cache of skipped message keys `{index -> mk}`.
- On receiving index `i` in the current epoch:
  - `i == next`: derive `mk`, decrypt, advance, `next++`.
  - `next < i <= next + MAX_SKIP`: derive and cache `mk[next..i-1]`, decrypt
    with `mk_i`, advance `next = i+1`. `MAX_SKIP = 64` (proposed; a tuning
    constant, not a security boundary, bounded by review).
  - `i > next + MAX_SKIP`: **refuse.** Do not derive that many keys. Drop the
    frame. This is the DoS bound: an attacker naming a far-future index buys at
    most `MAX_SKIP` derivations, not unbounded work.
  - `i < next` and in the skip cache: decrypt with the cached `mk`, evict it
    (each skipped key is single-use).
  - `i < next` and not in the cache: drop as replay/too-old. (The existing
    per-sender replay window on the nonce counter, `components/replay_window`,
    remains the authoritative replay defense; the ratchet index is an ordering
    aid, not a second replay oracle.)
- Skip cache is a fixed-size LRU (proposed 64 entries per direction). Eviction
  under load loses the ability to decrypt one specific reordered straggler,
  which then triggers the existing decrypt-failure path.
- **Epoch transition:** keep the previous epoch's receive chain and skip cache
  for a bounded grace (proposed: until `MAX_SKIP` messages seen on the new
  epoch, or a short timer), so in-flight old-epoch messages still decrypt, then
  wipe them. Wiping the old root/chains is what delivers PCS.

**Bound-exceeded behavior ties into desync-heal.** When a receiver cannot
decrypt (index too far ahead, cache evicted, epoch mismatch it cannot
reconcile), it returns the same decrypt failure `handle_data` already handles,
which fires `maybe_trigger_dm_rehandshake` (commit `8ab55838`). The ratchet
adds no new recovery mechanism; it degrades into the one that already exists.
This is the single most important compositional property of the design: **the
ratchet's worst case is the desync-heal path, already built, already
rate-limited, already neighbor-gated.**

## A.5 Interaction with `ke_epoch` and desync-heal, spelled out

- `ke_epoch` stops being a vestigial always-0 field and becomes the DH-ratchet
  epoch counter. The 1-byte `key_id` on the wire carries its low byte, matching
  the 1-byte `epoch` in the ratchet header; the full `ke_epoch` is 16-bit in
  `dm_session_t` and wraps far outside any realistic session lifetime (a
  wrap forces a full re-handshake, which is fine).
- A desync-heal re-handshake (`maybe_trigger_dm_rehandshake`) produces a fresh
  IKM and resets to epoch 0. FS is preserved because the old root and chains
  are wiped on teardown (`dm_session_teardown` already memsets the slot). The
  only change needed: ensure teardown wipes the *new* chain/skip state too, not
  just `session_key`.
- The proactive DH ratchet (epoch bump) and the reactive desync-heal
  (re-handshake) are the same handshake machinery with different triggers, so
  they cannot fight: whichever completes last wins the session state, and the
  loser degrades to a decrypt failure that re-heals.

## A.6 Migration, negotiation, downgrade protection

**Recommendation: hard wire flag day, ratchet mandatory in the new protocol
version. No negotiation.** This matches the project's established culture
(`BRAMBLE_VERSION` bumps are strict `==` flag days with "no compatibility
shim", per `packet.h` and the wire-v4 note in `SECURITY-MODEL.md`).

- Bump `BRAMBLE_VERSION`. New nodes speak only the ratchet; old frames are
  dropped at the RX version gate, same as every prior bump.
- Because there is no negotiation, there is **no downgrade surface**: an active
  MitM cannot strip a "supports FS" flag to force the static-key path, because
  the static-key path no longer exists in the new version.
- Existing sessions do not migrate in place: on the flag day they fail to
  decrypt once and re-handshake into a ratcheting session via the existing
  desync-heal path. This is the same one-time reconnect every prior flag day
  imposed.
- **Rejected alternative: capability negotiation** (advertise FS support in the
  INIT, both-support gates ratcheting). This keeps interoperability across the
  flag day but reintroduces a downgrade surface. It is only defensible by
  binding the advertised capability into the `K_confirm` transcript (which
  already covers the handshake fields, so a stripped flag would fail the
  confirmation tag). Given the project ships flag days routinely, the hard cut
  is simpler and strictly safer. **Decision for the user:** confirm the hard
  flag day is acceptable versus a negotiated rollout.

---

# Part B: SAS-comparison UX

## B.1 What the SAS should commit to (the key decision)

Today `dm_derive_sas` commits to the full 128-byte session IKM, which includes
the ephemerals (DH1). Consequence: **the SAS changes every time the session is
re-handshaked** (desync-heal, or a Part A epoch bump if it fed the IKM). A
`verified` flag tied to that SAS would be silently invalidated by a reboot or a
desync-heal, forcing re-verification for no security reason. That is a bad UX
and, worse, trains users to click through re-verification prompts.

**Recommendation: redefine the SAS to commit to the two long-term identity
keys, not the session.** Concretely, a "safety-number" style SAS:

```
SAS = decimal_7( HKDF(salt="bramble-sas-id", ikm = "",
                       info = id_lo_x25519 || id_hi_x25519, L=4) )
```

where `id_lo`/`id_hi` are the two peers' pinned X25519 identity keys in
`addr_lo`/`addr_hi` order (optionally also mixing the Ed25519 identity keys,
since the address already binds to Ed25519). This is public-key material, so the
SAS is a *fingerprint*, not a secret, exactly like Signal's safety numbers.

Why this is correct rather than a weakening:

- The purpose of the SAS is to detect a MitM who substituted keys during the
  unauthenticated first-contact window. If a MitM sat in the middle, my pinned
  peer key is the *attacker's* key, so my SAS reflects the attacker's key; the
  peer's SAS reflects their real key (or the attacker's substituted key toward
  them). Comparing out of band, the two differ. MitM detected. The ephemeral
  DH adds nothing to this detection: DM continuity (`dm_verify_init`/`_resp`
  pin enforcement) already forces every handshake's `long_term_pubkey` to equal
  the pinned identity key, so binding the SAS to that pinned key is exactly what
  the user is being asked to confirm.
- It is **stable across ratchet steps, epoch bumps, desync-heal, and reboot**,
  because it depends only on the two identity keys, which are pinned and
  attestation-bound. A ratchet step changes session state; it does not change
  who you are talking to, so it must not change the SAS. This is the property
  the team lead asked to reason toward, and it holds.

**Flag for review, and a decision for the user:** this changes what the SAS
commits to (session -> identity). It is the standard safety-number model and is
what makes verified-state durable, but it is a semantic change to a shipped
(if unsurfaced) primitive and should be reviewed alongside Part A. Keep
`dm_derive_sas` (session-IKM) available if review wants the SAS to additionally
confirm the specific first handshake; the two are not mutually exclusive, but
the *persisted verified state* must key on the identity SAS to be durable.

## B.2 Verification flow and verified-state persistence

- **Verified state lives on the TOFU pin, not the session.** Add a
  `verified` bit (and the SAS-at-verification-time) to the identity pin entry
  (`components/identity/identity_store.c`), keyed on `{address, x25519_pub}`.
  Because it keys on the pinned identity key, it survives reboot only if the
  pin store is persisted; the pin store is RAM-only today
  (`SECURITY-MODEL.md`), so verified state is RAM-only too unless the pin
  store is made persistent. **Decision for the user:** persist the pin store
  (and verified bits) to NVS, or accept that verification must be redone after
  reboot. Persisting is the right call for a usable "verified once, stays
  verified" model; it is a separable change from this spec.
- **Marking verified.** Two users compare the 7-digit SAS out of band (in
  person, voice, another channel). When they match, each marks the peer
  verified. The `dm_session_t.verified` flag (already present, already
  consulted by `dm_alloc`'s eviction protection) is set from the pin's verified
  bit on session (re)establishment.
- **Key change / rebind red flag.** If a pinned peer's identity key changes,
  the existing machinery already fires: `DM_VERIFY_ERR_PIN_MISMATCH` and
  `dm_pin_disagrees` -> `dm_session_teardown`. On top of that, this design
  **clears the verified bit** and surfaces a re-verification prompt: the SAS
  has changed because the identity key changed, which is precisely the event a
  user must be told about. A silent re-pin would defeat the purpose.

## B.3 Pager UX (250x122 e-paper, UP/DOWN/SELECT + RST)

The e-paper cannot show a rich UI; the flow is a small screen sequence driven
by three buttons.

- **Entry point.** In the per-peer DM view, an unverified peer shows a small
  "unverified" glyph in the header (a distinct mark, not an alarm). SELECT on
  the peer opens a context menu with a "Verify" item; UP/DOWN move the
  selection, SELECT confirms.
- **SAS screen.** Full-screen, large digits: the 7-digit SAS grouped `XXX XXXX`
  for readability, the peer's short address, and one line of instruction
  ("Read this aloud. It must match on both devices."). UP/DOWN do nothing here
  (single screen); SELECT advances to the confirm prompt; RST/back cancels.
- **Confirm prompt.** "Codes match?" with two choices (Yes / No). SELECT on
  "Yes" sets the verified bit and returns to the DM view now showing a
  "verified" glyph; "No" leaves it unverified and shows a brief warning line
  ("Do not trust this contact"). Default selection is "No" (fail-safe: a user
  mashing SELECT does not accidentally verify).
- **Re-verification.** On a key-change red flag, the DM header glyph flips to a
  distinct warning mark and the next open of the DM view interstitials a "This
  contact's key changed. Re-verify." screen before showing messages.

The SAS must be identical on both pagers; because it is identity-bound (B.1),
it is stable and reproducible, so two users can compare at any time, not only
immediately after a handshake.

## B.4 Webapp UX (the richer surface)

- **Per-conversation verification panel.** A "Verify safety number" affordance
  in the DM header showing verification status (unverified / verified /
  key-changed). Opening it shows the 7-digit SAS large, plus a longer
  human-readable fingerprint rendering of the identity keys if desired, and a
  QR encoding of the SAS/fingerprint so two co-located phones can scan instead
  of read.
- **Compare flows:** (1) read the digits aloud, (2) scan the QR, (3)
  copy/paste over another secure channel. On a match the user clicks "Mark
  verified", which sets the pin's verified bit via an authenticated RPC.
- **Status surfacing.** The conversation list shows a verified badge per peer.
  A key change surfaces a prominent, non-dismissible-until-acknowledged banner
  in that conversation ("The safety number changed. This can happen if your
  contact reinstalled, or it can mean someone is intercepting. Re-verify before
  trusting."), mirroring the pager interstitial, and the badge flips to a
  warning state.
- **Tie into the pin store.** Verified state is read/written through the same
  identity-store RPC surface, so the pager and webapp agree on verification
  status for the same `{address, x25519_pub}` pin.

## B.5 How FS interacts with SAS

- **The SAS commits to identity keys, so ratcheting and epoch bumps do NOT
  force re-verification.** A Part A epoch bump changes the root and chains; it
  does not change either identity key, so the identity-bound SAS is invariant
  across it. This is by construction (B.1) and is the correct behavior: the
  user verified *who* they are talking to, and a key ratchet does not change
  who.
- **A desync-heal re-handshake does NOT force re-verification** either, for the
  same reason: it re-runs the handshake with the *same* pinned identity keys
  (the pin is what `dm_verify_init`/`_resp` enforce), so the identity SAS is
  unchanged. Only a genuine identity-key change (rebind red flag) changes the
  SAS and re-prompts.
- **Re-verification is required in exactly one case:** the pinned identity key
  changed (`DM_VERIFY_ERR_PIN_MISMATCH` / `dm_pin_disagrees`). That is the one
  event that both tears down the session (existing behavior) and clears the
  verified bit (new behavior), because it is the one event where the SAS
  genuinely differs and the user genuinely must re-check.

---

# Testing and verification strategy

- **Host tests (ratchet KDF schedule), `test/`.** Deterministic KAT-style
  vectors for the root/chain/message-key derivations: fixed IKM in, fixed
  `RK_0`, `CK_0[lo->hi]`, `CK_0[hi->lo]`, `mk_0..mk_k` out. Both host backends
  (OpenSSL) must agree, pinned by committed vectors, exactly as
  `test/test_ed25519.c` pins the Ed25519 vectors. A cross-implementation
  agreement test: two simulated peers derive the same directional chains from
  the same handshake.
- **Host tests (skipped-key logic).** In-order, out-of-order within `MAX_SKIP`,
  reorder across the boundary, `i > next + MAX_SKIP` refusal, skip-cache
  eviction, replay of an already-consumed index, and epoch transition with
  in-flight old-epoch messages. Assert the bound is never exceeded (the DoS
  property) and that a bound breach returns the *decrypt-failure* signal, not a
  crash.
- **Host tests (SAS identity-binding).** Same identity keys -> same SAS across
  different session IKMs (stability); different identity keys -> different SAS
  (MitM detection); order independence (`addr_lo`/`addr_hi` canonicalization).
- **Desync-heal composition test.** Drive a session past `MAX_SKIP`, assert it
  triggers the existing `maybe_trigger_dm_rehandshake` path and recovers, and
  that FS holds (old message keys are wiped and unrecoverable from post-heal
  state).
- **Emulator (UX), AVD `bramble-e2e` and the pager device view.** Two virtual
  pagers on the gosim ether: establish a DM, verify the SAS matches on both,
  mark verified on both, confirm the verified glyph/badge, then force an
  identity-key change and confirm both surfaces show the re-verification prompt
  and the verified bit clears. Per `CLAUDE.md`, verify against the real
  rendered device view / WebView over CDP, not screenshots.
- **`SECURITY-MODEL.md` update in the same PR** (house rule: the doc changes
  with the code). Asset 4's "There is no forward secrecy" and section 5's "DM
  handshake SAS verification has no UX" residual both change; state precisely
  what closes (per-message FS for DMs; SAS UX on both surfaces) and what does
  not (PCS is coarse/epoch-bounded, the network-key insider, the
  first-contact TOFU window, RAM-only pins unless persistence lands).

---

# Residuals and review gates

**Honest residuals (must land in `SECURITY-MODEL.md`):**

- **PCS is coarse, not per-message.** A compromise heals only at the next
  epoch bump (bounded by N messages or T minutes), not immediately. This is the
  deliberate LoRa tradeoff (A.4).
- **The network-key insider is unchanged.** FS protects DM content, not the
  fact of the DM, its timing, size, or `src_addr`, all of which the shared
  flood still exposes.
- **The first-contact TOFU window is unchanged.** FS gives you a forward-secret
  channel to whoever you handshaked with; the SAS is what tells you that was
  the right person, and only if the users actually compare it.
- **Verified state and pins are RAM-only** unless the pin store is persisted to
  NVS (B.2). Without persistence, verification is redone after reboot. With
  persistence, note that identity keys already sit in plaintext NVS
  (device-thief adversary), so verified state is no more exposed than the keys
  it certifies.
- **Skip-cache and replay-table bounds** are DoS-shaped, not
  confidentiality-shaped: an evicted straggler or a `MAX_SKIP`-exceeding gap
  degrades to a re-handshake, never to plaintext exposure.

**Needs external cryptographic review before ship (blocking):**

1. The full key schedule (root KDF, directional chain split, message-key
   derivation, DH-ratchet root chaining) as an instance of the Double Ratchet,
   specifically the **decoupled/scheduled DH ratchet** variant and its exact
   PCS-latency bound.
2. The directional-chain labelling by address order for a **role-symmetric**
   handshake (the existing design is symmetric, not initiator/responder
   asymmetric like Signal); confirm no reflection/unknown-key-share issue is
   introduced by deriving two directional chains from one symmetric root.
3. The epoch-transition state machine: the grace-window for old-epoch in-flight
   messages, the wipe ordering (PCS depends on wiping the old root/chains at the
   right moment), and the interaction with the two concurrent rekey triggers
   (proactive epoch bump vs reactive desync-heal).
4. The **SAS redefinition** from session-IKM to identity keys (B.1): confirm the
   safety-number model is the intended MitM-detection semantics here and that
   dropping the ephemeral binding loses nothing given pin enforcement.
5. Nonce discipline under the ratchet: message keys change per message, but the
   GCM nonce is still the node-global counter (`nonce_counter`). Confirm that a
   fresh key per message plus a unique nonce per message is sound, and that a
   nonce-counter reuse across a key change cannot arise (it cannot today because
   the counter is monotonic and fail-closed, but review should confirm the
   key/nonce independence explicitly).

**Decisions for the user:**

- Hard wire flag day (ratchet mandatory, no negotiation, no downgrade surface)
  versus a negotiated rollout with capability binding in the transcript (A.6).
  Recommendation: hard flag day, matching project culture.
- Persist the identity pin store (and verified bits) to NVS so verification is
  durable across reboot (B.2). Recommendation: yes; it is what makes "verified"
  mean something day to day.
- Whether the SAS should additionally retain the session-IKM binding for
  first-handshake confirmation, or move fully to identity-bound (B.1).
  Recommendation: persisted verified state keys on the identity SAS; keep the
  session SAS only if review wants belt-and-suspenders on the first handshake.
