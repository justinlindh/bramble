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
 * NOT in scope this phase (Phase 4): no address rebind, no gating of any
 * operation on pin state. identity_store_lookup is the query surface
 * Phase 4 builds on.
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
     * did not verify, i.e. a keyed member sent garbage. */
    uint32_t conflicts;
    uint32_t sig_failures;
} identity_store_t;

typedef enum {
    IDENTITY_PIN_NEW = 0,   /* first verified binding for this address: stored */
    IDENTITY_PIN_REFRESHED, /* identical to the pinned binding: LRU refresh only */
    IDENTITY_PIN_CONFLICT,  /* different keys for a pinned address: REJECTED */
    IDENTITY_PIN_SELF,      /* our own address: ignored */
    IDENTITY_PIN_BAD_SIG,   /* Ed25519 signature invalid: not pinned */
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

/* Number of used entries (diagnostics). */
int identity_store_count(const identity_store_t* s);

#endif /* BRAMBLE_IDENTITY_STORE_H */
