#ifndef BRAMBLE_IDENTITY_STORE_H
#define BRAMBLE_IDENTITY_STORE_H

#include <stdbool.h>
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
 *   - RAM only this phase (residual): pins reset on reboot and TOFU
 *     re-establishes; NVS persistence is Phase 4+ material.
 *
 * Phase 4 (address rebind): the node address derives from the Ed25519
 * identity key, and identity_store_handle_attestation additionally
 * REQUIRES src_addr == crypto_derive_address(ed25519_pub), rejecting
 * mismatched claims even on first contact (IDENTITY_PIN_ADDR_MISMATCH).
 * identity_store_lookup is the query surface the Phase 4 gates
 * (timesync-quorum eligibility, DM key continuity) build on.
 */

#define IDENTITY_STORE_CAPACITY 32

typedef struct {
    bool used;
    uint32_t address;
    uint8_t ed25519_pub[32];
    uint8_t x25519_pub[32];
    uint32_t pinned_at_ms;      /* when the binding was first stored */
    uint32_t last_confirmed_ms; /* last identical re-attestation (LRU key) */
} identity_pin_t;

typedef struct {
    identity_pin_t entries[IDENTITY_STORE_CAPACITY];
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
} identity_pin_result_t;

void identity_store_init(identity_store_t* s);

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
 */
identity_pin_result_t identity_store_handle_attestation(identity_store_t* s,
                                                        const bramble_identity_attestation_t* att,
                                                        uint32_t self_addr, uint32_t now_ms);

/* Phase 4 query surface: the pinned entry for address, or NULL. */
const identity_pin_t* identity_store_lookup(const identity_store_t* s, uint32_t address);

/*
 * Phase 4 timesync-quorum gate: whether a peer may count toward the
 * pre-commit corroboration quorum (timesync_handle_sync's
 * source_established input, ws 1.3c / NEW-SEC-4).
 *
 * CHOSEN SEMANTIC (documented here, tested in test_identity_store.c):
 *   eligible = established AND (pinned OR store holds ZERO pins).
 *
 * - `established` is the existing neighbor-tenure signal
 *   (neighbor_is_established); it is ALWAYS required, never relaxed.
 * - Once ANY verified identity is pinned, only PINNED peers corroborate
 *   time: an insider fabricating fresh source addresses can no longer
 *   quorum the clock, because a fabricated address cannot be pinned at
 *   all post-rebind (it would need the deriving Ed25519 key).
 * - With ZERO pins the gate falls back to tenure alone: a fresh mesh (or
 *   any node right after boot, since pins are RAM-only) must still
 *   converge with no attestations heard yet. Degrade, never brick.
 *   Transient window accepted: between the first pin arriving and the
 *   rest of the neighbors' attestations (boot-hook + 15 min cadence),
 *   unpinned established neighbors drop out of the quorum; the
 *   post-commit timesync path (stratum/shift gates) is unaffected.
 * - Unpinned peers lose ONLY quorum membership; they remain neighbors,
 *   relays and DM peers.
 */
bool identity_store_quorum_eligible(const identity_store_t* s, uint32_t address, bool established);

/* Number of used entries (diagnostics). */
int identity_store_count(const identity_store_t* s);

#endif /* BRAMBLE_IDENTITY_STORE_H */
