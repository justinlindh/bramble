#ifndef BRAMBLE_IDENTITY_STORE_H
#define BRAMBLE_IDENTITY_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "packet.h" /* bramble_identity_attestation_t */

/*
 * Verified TOFU identity pin store (per-node identity Phase 3, Part C).
 *
 * Every node builds a verified table of its mesh: address -> {Ed25519
 * pub, X25519 pub}, populated from delivered (relay-gate-MAC-valid,
 * replay-fresh) identity attestations whose Ed25519 signature verifies
 * against the frame's own embedded key. Trust-on-first-use: the FIRST
 * verified binding for an address wins; a later attestation for the same
 * address under DIFFERENT keys is a CONFLICT and is REJECTED, keeping the
 * original binding. That refusal is the campaign's impersonation
 * detection: a keyed insider attesting a victim's address with its own
 * key can produce an internally valid frame, but it can never displace
 * the victim's established binding at any node that heard the victim
 * first.
 *
 * Deliberately a caller-owned struct (no static singleton): mesh_task.c
 * owns one instance for the firmware, host tests own theirs, and gosim
 * owns one per simulated node.
 *
 * Bounds and lifetime:
 *   - IDENTITY_STORE_CAPACITY entries (32, the neighbor-table scale).
 *   - Eviction is LRU by last_confirmed_ms: a re-heard IDENTICAL
 *     attestation refreshes its entry (attestations are replayed on a
 *     cadence by design), so live bindings stay; only bindings nothing
 *     has confirmed lately get displaced. A CONFLICT does not refresh.
 *   - The RAM store is authoritative; the binding + its verified bit and
 *     SAS-at-verification also persist to NVS (identity_store_serialize /
 *     _deserialize, driven by mesh_task.c) so a "verified once, stays
 *     verified" model survives reboot. The LRU bookkeeping (pinned_at_ms /
 *     last_confirmed_ms) is not persisted and legitimately resets on reboot.
 *
 * Phase 4 (address rebind): the node address derives from the Ed25519
 * identity key, and identity_store_handle_attestation additionally
 * REQUIRES src_addr == crypto_derive_address(ed25519_pub), rejecting
 * mismatched claims even on first contact (IDENTITY_PIN_ADDR_MISMATCH).
 * identity_store_lookup is the query surface the Phase 4 gates
 * (timesync-quorum eligibility, DM key continuity) build on.
 */

#define IDENTITY_STORE_CAPACITY 32

/*
 * Bootstrap-quorum grace (NEW-SEC-4 "1.3c" race close). Within this window
 * after THIS node's boot, an established-but-unpinned peer may still count
 * toward the timesync corroboration quorum, so a fresh mesh with no verified
 * attestations pinned yet can still bootstrap its clock (liveness). After
 * the window ONLY pinned peers ever corroborate: an unattested or Sybil node
 * can no longer dominate the quorum and skew the mesh clock (the race is
 * closed).
 *
 * Tradeoff on the value: a LONGER grace gives more liveness margin on large
 * or slow meshes where propagating and verifying the first attestations
 * takes longer, at the cost of a wider window in which an unattested peer
 * could skew time; a SHORTER grace tightens that exposure but risks a slow
 * mesh failing to bootstrap timesync at all. 5 minutes is a backstop, not
 * the normal path: every node attests on boot (immediate) and every 15 min,
 * so genuine pins normally arrive within seconds-to-minutes and the gate has
 * already tightened to pinned-only well before the grace expires.
 */
#define QUORUM_BOOTSTRAP_GRACE_MS 300000u

/*
 * Bootstrap-grace early-exit threshold (trust-anchor P3a). Once this node
 * holds at least this many verified pins it has real corroboration from
 * PINNED peers alone, so the unpinned-peer fallback is no longer needed: an
 * unpinned peer stops being grace-eligible the instant the pin count reaches
 * this floor, even if the QUORUM_BOOTSTRAP_GRACE_MS time window is still open.
 * The time bound stays as a backstop for the not-yet-3-pins case.
 *
 * Tradeoff on the value: a HIGHER threshold keeps the unpinned fallback open
 * longer (more liveness margin before enough pins arrive) at the cost of a
 * wider residual window; a LOWER threshold tightens to pinned-only sooner but
 * risks closing the fallback before a slow mesh has corroboration. 3 is a
 * small quorum: it is enough real corroboration to no longer need the unpinned
 * fallback, while a single lucky pin (1) would close the grace too eagerly.
 *
 * This early-exit is anchor-INDEPENDENT: it also tightens a NO-anchor TOFU
 * mesh to pinned-only once 3 TOFU pins exist. That is a deliberate strict
 * improvement (real corroboration removes the need for the unpinned fallback
 * whether or not the mesh is anchored), and a documented behavior change to
 * the #131 grace for ALL meshes, not just anchored ones.
 *
 * A companion negative-intel "reject ring" (bar an address that attested and
 * failed endorsement from the grace) was considered and DROPPED by the P3a
 * red-team: it gives no durable protection (a self-revealing Sybil un-rings
 * itself for free by cycling 16 fresh keypairs through the FIFO) AND it is a
 * net-new targeted-denial lever against HONEST peers (a keyed insider who
 * grinds a ~2^32 address collision can push a victim's address into the ring
 * and deny it timesync-quorum eligibility during the bootstrap window). The
 * early-exit has neither problem, so it stands alone.
 */
#define QUORUM_GRACE_MIN_PINS 3u

typedef struct {
    bool used;
    uint32_t address;
    uint8_t ed25519_pub[32];
    uint8_t x25519_pub[32];
    uint32_t pinned_at_ms;      /* when the binding was first stored */
    uint32_t last_confirmed_ms; /* last identical re-attestation (LRU key) */
    /* SAS verification state (DM forward-secrecy + SAS). The bit and the
     * SAS-at-verification-time persist to NVS with the pin (see
     * identity_store_serialize) so a "verified once, stays verified" model
     * survives reboot. Because the verified state keys on the pinned identity
     * key, ratchet steps, epoch bumps, desync-heal, and reboot never force
     * re-verification; only a pin key change (a CONFLICT / rebind) clears it. */
    bool verified;        /* SAS confirmed out of band, survives reboot via NVS */
    char verified_sas[8]; /* the 7-digit identity SAS at verification time + NUL */
    /* RAM-only, NOT serialized: a genuine identity-key change was seen and not
     * yet re-verified. Set by identity_store_mark_key_changed (Task 7's
     * verified_cleared branch in mesh_task.c, the ONE genuine key-change site),
     * cleared by identity_store_set_verified (re-verifying dismisses the
     * warning). Deliberately excluded from serialize/deserialize: the
     * security-critical persisted state is the verified bit; the key-change
     * warning framing matters most during the live session, and losing it
     * across reboot avoids re-versioning the blob. */
    bool key_changed;
} identity_pin_t;

typedef struct {
    identity_pin_t entries[IDENTITY_STORE_CAPACITY];
    /* This node's boot reference (identity_store_init's now_ms). The
     * bounded bootstrap-quorum grace is measured from here. Unlike the
     * pins, which persist to NVS, boot_ms is per-boot, so the grace
     * window re-opens on every reboot. */
    uint32_t boot_ms;
    /* Diagnostics counters (impersonation signal): conflicts counts
     * rejected re-bind attempts against a pinned address; sig_failures
     * counts delivered (MAC-valid) attestations whose Ed25519 signature
     * did not verify, i.e. a keyed member sent garbage; addr_mismatches
     * counts delivered attestations whose src_addr is not the address
     * their own Ed25519 key derives to (Phase 4 rebind), i.e. a keyed
     * member tried to attest an address it does not hold the key for. */
    uint32_t conflicts;
    uint32_t sig_failures;
    uint32_t addr_mismatches;
    /* Trust-anchor campaign (P2): when has_anchor is set this node pins ONLY
     * anchor-endorsed identities; anchor_pub is the fleet anchor's Ed25519
     * public key the endorsement is verified against. has_anchor false (the
     * default from identity_store_init) = NOT anchored = today's TOFU
     * behavior, bit-for-bit: the endorsement gate is skipped entirely and
     * cert fields on the wire are ignored. The store stays PURE: the anchor
     * is pushed in via identity_store_set_anchor (never read from NVS here),
     * and the wall-clock epoch for the expiry check is a call parameter
     * (never a timesync read here). unendorsed counts delivered attestations
     * refused for a missing/invalid endorsement; expired counts ones refused
     * because the cert's not_after has passed the synced wall clock. */
    bool has_anchor;
    uint8_t anchor_pub[32];
    uint32_t unendorsed;
    uint32_t expired;

    /* Issue #88 observability, RAM-only and not serialized. `evictions`
     * counts pins recycled to make room for a new one. `evictions_verified`
     * counts the subset that were SAS-verified pins, which is the case that
     * downgrades a human-confirmed binding back to trust-on-first-use and is
     * therefore worth surfacing rather than doing silently. alloc_entry only
     * ever touches a verified pin when EVERY pin is verified, so a climbing
     * evictions_verified means the store is genuinely at capacity, not that
     * an attestation flood is washing verified pins out. */
    uint32_t evictions;
    uint32_t evictions_verified;
    /* Trust-anchor campaign (P3a/P4b): set true when a runtime anchor CHANGE
     * dropped existing pins (a re-hardening of a node that had already
     * accumulated pins). While set, the bootstrap-quorum grace's unpinned
     * fallback is force-closed: a re-hardened node must corroborate timesync
     * from ENDORSED pins only, never from the unpinned window it would
     * otherwise re-open by having its pin count reset to 0 inside the boot
     * grace. It stays false through the boot-time FIRST anchoring (count 0, no
     * pins dropped), so a fresh anchored mesh keeps the bootstrap grace it
     * needs for liveness. Reset to false by the memset in identity_store_init. */
    bool grace_forced_closed;
} identity_store_t;

typedef enum {
    IDENTITY_PIN_NEW = 0,   /* first verified binding for this address: stored */
    IDENTITY_PIN_REFRESHED, /* identical to the pinned binding: LRU refresh only */
    IDENTITY_PIN_CONFLICT,  /* different keys for a pinned address: REJECTED */
    IDENTITY_PIN_SELF,      /* our own address: ignored */
    IDENTITY_PIN_BAD_SIG,   /* Ed25519 signature invalid: not pinned */
    /* src_addr != crypto_derive_address(ed25519_pub): rejected even on
     * first contact. The Phase 4 payoff: an insider cannot attest an
     * address without holding the key it hashes from (SHA256 preimage),
     * so address impersonation is cryptographically infeasible rather
     * than merely losing the TOFU race. */
    IDENTITY_PIN_ADDR_MISMATCH,
    /* Trust-anchor campaign (P2), fire ONLY on an anchored node (has_anchor):
     * UNENDORSED = the attestation carried no cert (not_after == 0) or one
     * that does not verify against our anchor for this exact ed25519_pub (a
     * missing, wrong-anchor, or cross-node-grafted cert); EXPIRED = the cert
     * verified but its not_after has passed the synced wall clock. Both gate
     * PINNING only: the relay/flood path is untouched, so an anchored relay
     * still forwards an unendorsed neighbor's MAC-valid frame, it just does
     * not pin it. A node with no anchor never returns either code. */
    IDENTITY_PIN_UNENDORSED,
    IDENTITY_PIN_EXPIRED,
} identity_pin_result_t;

/* now_ms is recorded as this node's boot reference for the bootstrap-quorum
 * grace (see QUORUM_BOOTSTRAP_GRACE_MS / identity_store_quorum_eligible).
 * Tests control the clock by choosing this value. */
void identity_store_init(identity_store_t* s, uint32_t now_ms);

/*
 * Raw TOFU pin of an already-VERIFIED binding (callers must have checked
 * the Ed25519 signature; identity_store_handle_attestation below does).
 * Returns NEW/REFRESHED/CONFLICT per the TOFU rules above. On CONFLICT
 * the stored entry is untouched (keys, pinned_at AND last_confirmed: a
 * conflicting frame is not a confirmation and must not move the entry's
 * LRU position).
 */
identity_pin_result_t identity_store_pin(identity_store_t* s, uint32_t address,
                                         const uint8_t ed25519_pub[32],
                                         const uint8_t x25519_pub[32], uint32_t now_ms);

/*
 * Full delivery path for a MAC-valid, replay-fresh attestation (the relay
 * gate in mesh_task.c / gosim's bridge runs BEFORE this): ignore self,
 * verify the Ed25519 signature over the canonical message
 * (bramble_identity_attestation_signed_msg) against the frame's embedded
 * ed25519_pub, then TOFU-pin. This is the ONLY place the expensive
 * Ed25519 verify runs on the receive side; relays never call it.
 *
 * now_ms is MONOTONIC boot-relative ms (LRU + bootstrap grace). epoch_ms is
 * the current network WALL-CLOCK in ms, or 0 when the clock is not
 * synced/confident: it is used ONLY for the endorsement expiry check on an
 * anchored node, and only when non-zero (an unsynced clock never expires a
 * cert; a permanent cert is unaffected either way). The store never reads
 * timesync itself: the caller passes epoch_ms.
 *
 * On an ANCHORED node (identity_store_set_anchor was called) the endorsement
 * gate runs AFTER the existing self/addr/self-sig checks and BEFORE the TOFU
 * pin: a missing/invalid cert -> IDENTITY_PIN_UNENDORSED, an expired one ->
 * IDENTITY_PIN_EXPIRED, neither pinned. On a node with no anchor the gate is
 * skipped and behavior is identical to before this parameter existed.
 */
identity_pin_result_t identity_store_handle_attestation(identity_store_t* s,
                                                        const bramble_identity_attestation_t* att,
                                                        uint32_t self_addr, uint32_t now_ms,
                                                        uint64_t epoch_ms);

/*
 * Trust-anchor campaign (P2): mark this store ANCHORED and record the fleet
 * anchor's Ed25519 public key. After this call identity_store_handle_
 * attestation pins ONLY identities the anchor has endorsed. Pure: copies the
 * key into the caller-owned struct, reads no NVS. identity_store_init leaves
 * a store UN-anchored (has_anchor false), so the anchored behavior is strictly
 * opt-in; a store that never sees this call keeps today's TOFU behavior.
 */
void identity_store_set_anchor(identity_store_t* s, const uint8_t anchor_pub[32]);

/* Phase 4 query surface: the pinned entry for address, or NULL. */
const identity_pin_t* identity_store_lookup(const identity_store_t* s, uint32_t address);

/*
 * Phase 4 timesync-quorum gate: whether a peer may count toward the
 * pre-commit corroboration quorum (timesync_handle_sync's
 * source_established input, ws 1.3c / NEW-SEC-4).
 *
 * CHOSEN SEMANTIC (documented here, tested in test_identity_store.c):
 *   if (!established)                       -> false  (tenure never relaxed)
 *   else if (pinned)                        -> true   (always eligible)
 *   else if (within-grace && count<MIN_PINS)
 *                                           -> true   (bounded boot grace)
 *   else                                    -> false  (race closed)
 *
 * - `established` is the existing neighbor-tenure signal
 *   (neighbor_is_established); it is ALWAYS required, never relaxed.
 * - A PINNED peer is always eligible (subject to tenure): once we hold a
 *   peer's verified binding it corroborates time regardless of the grace.
 * - An UNPINNED peer is eligible ONLY inside the bounded per-boot grace,
 *   which P3a tightens with the count early-exit beyond the time window:
 *     (time)  now_ms - boot_ms < QUORUM_BOOTSTRAP_GRACE_MS -- the backstop,
 *             so a fresh mesh can bootstrap timesync before any pin exists;
 *     (count) fewer than QUORUM_GRACE_MIN_PINS pins held -- early-exit: once
 *             real pinned corroboration exists the unpinned fallback closes,
 *             typically well before the time bound. Anchor-INDEPENDENT (it
 *             tightens no-anchor TOFU meshes too: a documented change to the
 *             #131 grace for ALL meshes, see QUORUM_GRACE_MIN_PINS).
 *   After the grace (or once enough pins) an unpinned peer NEVER corroborates,
 *   even with zero pins held. That closes NEW-SEC-4's bootstrap-quorum race:
 *   an unattested or Sybil node can no longer dominate the quorum and skew the
 *   mesh clock once the window ends.
 * - RESIDUAL (documented, inherent, NOT closed by P3a): while the grace is
 *   open and the node holds < MIN_PINS pins, an established UNPINNED peer --
 *   silent OR self-revealing Sybil alike -- can still ride the bounded window.
 *   A negative-intel reject ring to bar self-revealing Sybils was considered
 *   and DROPPED (flushable for free by cycling fresh keypairs, and a targeted
 *   deny lever against honest peers; see QUORUM_GRACE_MIN_PINS). The bounded
 *   window is the accepted residual; it is bounded by BOTH the time backstop
 *   and the 3-pin early-exit, whichever comes first.
 * - Because every node attests on boot + every 15 min, genuine pins arrive
 *   within seconds-to-minutes, so the count early-exit has typically already
 *   tightened the gate to pinned-only well before the grace expires; the
 *   grace is a liveness backstop, not the normal path.
 * - Unpinned peers lose ONLY quorum membership; they remain neighbors,
 *   relays and DM peers.
 */
bool identity_store_quorum_eligible(const identity_store_t* s, uint32_t address, bool established,
                                    uint32_t now_ms);

/* Number of used entries (diagnostics). */
int identity_store_count(const identity_store_t* s);

/*
 * SAS verification state (DM forward-secrecy + SAS). The verified bit lives on
 * the TOFU pin, not on the DM session, so it is stable across ratchet steps,
 * epoch bumps, desync-heal, and reboot; only a pin key change invalidates it.
 *
 * set_verified records the bit AND the SAS string the users compared out of
 * band (identity_store_serialize persists both); returns false if no pin exists
 * for address (verification must follow a pin). clear_verified drops the bit
 * (Task 7 calls it on a DM_VERIFY_ERR_PIN_MISMATCH key-change red flag) and
 * returns false if there is no pin. is_verified is the query surface for the UX
 * and the session snapshot.
 */
bool identity_store_set_verified(identity_store_t* s, uint32_t address, const char sas[8]);
bool identity_store_clear_verified(identity_store_t* s, uint32_t address);
bool identity_store_is_verified(const identity_store_t* s, uint32_t address);

/*
 * RAM-only key-change flag (see identity_pin_t.key_changed). Marks the peer's
 * pin as having seen a genuine identity-key change; returns false if there is
 * no pin for address. Callers must set this ONLY at the genuine key-change
 * site (a pin CONFLICT/rebind that also invalidated a verified DM session),
 * never from a deliberate user un-verify (identity_store_clear_verified is
 * also that path and does not touch key_changed). identity_store_key_changed
 * is the query surface for the UX; false if no pin or the flag is clear.
 */
bool identity_store_mark_key_changed(identity_store_t* s, uint32_t address);
bool identity_store_key_changed(const identity_store_t* s, uint32_t address);

/*
 * Pure (NVS-free) serialize/deserialize of the pin table so verification and
 * TOFU bindings survive reboot. The firmware caller (mesh_task.c) owns the
 * nvs_open/nvs_set_blob/nvs_commit around these, mirroring identity.c: the
 * IN-MEMORY store is authoritative, so a store-write failure never loses the
 * live pins. Only the durable fields are written (address, both pubkeys, the
 * verified bit, and the SAS); pinned_at_ms / last_confirmed_ms are LRU
 * bookkeeping that legitimately resets on reboot.
 *
 * serialize writes a 1-byte format version, a 1-byte used-entry count, then one
 * fixed record per used entry; returns the byte count written, or -1 if buf is
 * too small. deserialize FIRST re-initializes the pin table (so a caller may
 * pass a fresh/zeroed store) while PRESERVING any anchor already provisioned on
 * it, then rebuilds every record; returns 0 on success, -1 on a wrong version
 * byte or a truncated buffer (leaving the store initialized and empty).
 */
#define IDENTITY_STORE_BLOB_VERSION 1u
/* address(4) || ed25519_pub(32) || x25519_pub(32) || verified(1) || sas(8) */
#define IDENTITY_STORE_RECORD_SIZE (4 + 32 + 32 + 1 + 8)
/* version(1) || count(1) || CAPACITY records */
#define IDENTITY_STORE_BLOB_MAX (2 + IDENTITY_STORE_CAPACITY * IDENTITY_STORE_RECORD_SIZE)

int identity_store_serialize(const identity_store_t* s, uint8_t* buf, size_t buf_len);
int identity_store_deserialize(identity_store_t* s, const uint8_t* buf, size_t len,
                               uint32_t now_ms);

#endif /* BRAMBLE_IDENTITY_STORE_H */
