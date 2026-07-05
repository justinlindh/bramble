# Task 2 report: trust-surface inventory + honest docs + competitive delta (campaign close)

Status: DONE. Docs-only, one commit. No code change was needed (see below).

## What was done

Closed the mandatory-attestation campaign by inventorying the remaining
Sybil/unattested trust surfaces beyond the Task 1 timesync quorum, deciding
gate-or-justify for each, and rewriting the docs so they state precisely what
the bounded per-boot grace does and does not close. No overclaiming: the
bootstrap-quorum RACE is closed, Sybil MINTING is still free.

## Part A: trust-surface inventory + gate-or-justify

### Mailbox custody (primary candidate): LEFT OPEN, justified (not gated)

Verified against the code, not the brief:
- `mesh_mailbox_store` (`main/mesh_task.c`) is reached from `forward_data_packet`
  only after `data_auth_verify` passed at the RX dispatch gate (mesh_task.c
  ~:3711, strictly before `data_rx_decide`/`forward_data_packet` at ~:3747).
  So custody only ever stores a wire-v4 network-key-authenticated DATA frame.
- `s_mailbox_enabled` defaults false (NVS-gated, opt-in).
- Storage is a fixed static 32-entry array (`MAILBOX_MAX_ENTRIES`), capped 8
  per source (`MAILBOX_MAX_PER_SOURCE`) and 8 per destination
  (`MAILBOX_MAX_PER_DEST`), 24h TTL, LRU eviction, no dynamic allocation
  (`components/mailbox/mailbox.c`). Content is the raw (ciphertext) DATA.

Decision: NOT a meaningful Sybil/exhaustion lever, leave open. Reasoning:
1. Opt-in (default off).
2. Outsider-proof already: a keyless outsider cannot inject a custody entry at
   all; only a network-key insider can, which is the inherent shared-key
   residual, not a new surface.
3. Self-bounded: a single source (Sybil or not) can occupy at most 8 of 32
   slots; worst case is bounded eviction of other pending entries (an
   availability cost on an opt-in feature), never memory exhaustion.
4. Custody grants no trust: a stored entry is a deferred DATA packet
   re-transmitted verbatim; the destination independently re-authenticates and
   decrypts. Holding a message never feeds any gated trust decision.

Gating custody on a pinned source would HARM liveness (store-and-forward
exists for exactly the partitioned/bootstrapping case where a source's
attestation has not propagated) while buying no real protection: because Sybil
minting is free, an attacker clears a pin requirement by pinning fake
identities. A gate the attacker trivially clears, at a liveness cost, is worse
than no gate. Because nothing was gated, no mailbox test was added (there is no
new behavior to assert); the justification is documented in
`docs/SECURITY-MODEL.md` (Mailbox content section) and here.

### Other surfaces glanced (per brief), all left as-is

- Neighbor-table admission: transport-plane presence, not a trust decision.
  The only trust consequence (timesync quorum) is separately gated by
  `identity_store_quorum_eligible` + tenure, so evicting/adding neighbors does
  not buy quorum influence past the grace. Not gated (out of scope).
- Route/RREP trust: network-key-authenticated, not identity-gated. This
  campaign does not add identity gating to routing. Noted out of scope
  (residual 3 in NEW-SEC-4).
- Transport / DATA relay / routable-destination / first-contact DM: liveness,
  deliberately stay open. Untouched.

### Code comment (mesh_task.c:1412)

The brief flagged the `1.3c` deferred-race comment for update. Verified it was
already updated by Task 1 (commit ecf34b05): it now reads "the bootstrap-quorum
race (1.3c) is closed separately by the bounded per-boot grace in
identity_store_quorum_eligible", and the quorum call-site comment (~:1471)
already reflects the bounded grace. No further code change was needed, so this
task is docs-only.

## Part B: docs (honest) - SECURITY-MODEL.md NEW-SEC-4

Rewrote the NEW-SEC-4 residual (section 5) and every cross-reference to state:

CLOSED: the bootstrap-quorum RACE. The old UNBOUNDED "zero pins held, trust
every established peer" window is replaced by a bounded per-boot grace
(`QUORUM_BOOTSTRAP_GRACE_MS`, 5 min, measured from this node's boot); after the
grace an unpinned peer NEVER corroborates the quorum. Verified against
`identity_store_quorum_eligible` (established && (pinned || within-grace)) and
the RAM-only `boot_ms` pin-store reference.

The 3 residuals, stated precisely:
1. Sybil MINTING is still free: no trust anchor, no cost function; an insider
   can mint/attest/pin/sustain N real identities and win the quorum. This
   campaign does NOT create scarcity (the deferred trust-anchor campaign does).
2. The per-boot grace is a bounded residual exposure window: a Sybil
   established during a node's first 5 min post-boot can still corroborate as
   unpinned. Bounded per boot, not unbounded, but not zero.
3. Route/RREP trust is not identity-gated (out of scope).

Also stated the uniform-attestation principle: there is no UNATTESTED path into
the gated trust decisions (timesync quorum after the grace, DM key continuity);
every participant in a gated trust decision has at least attested and been
pinned. Did NOT claim Sybil is solved anywhere.

Cross-references updated for consistency (section 3 control-plane heading and
body, the section 3 residuals paragraph, the "Identity-gated timesync quorum"
bullet, the freshness paragraph, section 4's control-plane gap bullet, and the
Mailbox content section).

## Part C: competitive delta (docs/COMPARISON.md)

Added to the "Per-node identity delta" section: the mandatory-attestation
campaign closes the bootstrap-quorum RACE (unbounded zero-pins window ->
bounded 5-min per-boot grace); attestation is now a prerequisite for trusted
participation (no unattested path into the gated decisions), every Sybil
identity is a visible, counted, airtime-costing attestation, and the trivial
no-attestation-needed quorum attack is gone. Honest scope stated: bounded step,
not the Sybil solution; full scarcity awaits the trust anchor, not claimed.

## CI

Docs-only diff (docs/**, plus .superpowers scratch). No source, so the code
gates (host tests, board build, clang-format/cppcheck runner, check-rpc-
contract, gosim) are unaffected. SECURITY-MODEL.md is pure ASCII; COMPARISON.md
additions are ASCII; no em dash in either (verified by codepoint scan). The
em-dash pre-commit hook passes.

## Concerns

None blocking. The honest residual is unchanged and by design: Sybil minting
scarcity and the bounded per-boot grace window both remain open and are now
documented as such. Mailbox left open is a deliberate, defended decision.
