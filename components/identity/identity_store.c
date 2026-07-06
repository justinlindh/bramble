/*
 * Verified TOFU identity pin store (per-node identity Phase 3, Part C).
 * See identity_store.h for the trust model. Pure C over a caller-owned
 * struct: host-testable directly, one instance per node in gosim, one
 * static instance in mesh_task.c.
 */
#include "include/identity_store.h"

#include <string.h>

#include "crypto.h"           /* crypto_ed25519_verify */
#include "include/identity.h" /* identity_endorsement_verify (trust-anchor P2) */

void identity_store_init(identity_store_t* s, uint32_t now_ms) {
    memset(s, 0, sizeof(*s));
    s->boot_ms = now_ms;
    /* has_anchor stays false: a fresh store is NOT anchored, so the
     * endorsement gate is skipped and behavior is today's TOFU exactly.
     * Anchoring is opt-in via identity_store_set_anchor. */
}

void identity_store_set_anchor(identity_store_t* s, const uint8_t anchor_pub[32]) {
    /* Whether this call changes the effective anchor: first-ever anchoring, or a
     * rotation to a different key. An idempotent same-key re-provision is NOT a
     * change (P1 idempotency concern), so it must keep the endorsed pins. */
    bool changed = !s->has_anchor || memcmp(s->anchor_pub, anchor_pub, 32) != 0;
    /* Snapshot whether pins existed BEFORE we clear anything: this decides
     * whether the anchor change is a re-hardening (had pins, now dropping them)
     * or the boot-time first anchoring (no pins yet). */
    bool had_pins = identity_store_count(s) > 0;

    /* memcpy the key BEFORE raising has_anchor: a concurrent reader (the mesh
     * task in handle_attestation) must never see has_anchor=true over a
     * half-copied key. */
    memcpy(s->anchor_pub, anchor_pub, 32);
    s->has_anchor = true;

    /* When the anchor changes, DROP every pin: the un-endorsed TOFU pins a node
     * accumulated while un-anchored (or endorsed pins under the OLD anchor after
     * a rotation) are not endorsed under the new key, so keeping them would be
     * exactly "an un-endorsed identity pinned on an anchored node". Clearing
     * entries here is what makes runtime setAnchor actually harden the store,
     * not just future attestations. boot_ms and the counters are NOT reset (the
     * diagnostic counters are cumulative); idempotent same-key re-set leaves the
     * endorsed pins in place.
     *
     * Bootstrap-grace interaction (P4b): clearing the pins resets the pin count
     * to 0, which would by itself RE-OPEN the timesync bootstrap grace's
     * unpinned fallback (the count < QUORUM_GRACE_MIN_PINS clause flips true
     * again) whenever this runs inside the per-boot grace window. To stop a
     * runtime re-hardening from re-extending unpinned trust, force the grace
     * closed -- but ONLY when the change actually DROPPED EXISTING pins
     * (had_pins). The boot-time FIRST anchoring has count 0, so had_pins is
     * false and the grace is preserved: a fresh anchored mesh legitimately needs
     * the unpinned window to bootstrap timesync before endorsed pins propagate
     * (liveness). Once real pins existed and a runtime anchor change wipes them,
     * the node has been hardened and must corroborate from ENDORSED pins only. */
    if (changed) {
        for (int i = 0; i < IDENTITY_STORE_CAPACITY; i++)
            s->entries[i].used = false;
        if (had_pins)
            s->grace_forced_closed = true;
    }
}

static identity_pin_t* find_entry(identity_store_t* s, uint32_t address) {
    for (int i = 0; i < IDENTITY_STORE_CAPACITY; i++) {
        if (s->entries[i].used && s->entries[i].address == address)
            return &s->entries[i];
    }
    return NULL;
}

/* A free slot if one exists, else the least-recently-confirmed entry
 * (LRU eviction victim). Age is computed as now - last_confirmed with
 * uint32 wraparound semantics, the same idiom the rest of the codebase
 * uses for now_ms arithmetic. */
static identity_pin_t* alloc_entry(identity_store_t* s, uint32_t now_ms) {
    identity_pin_t* victim = NULL;
    uint32_t victim_age = 0;
    for (int i = 0; i < IDENTITY_STORE_CAPACITY; i++) {
        if (!s->entries[i].used)
            return &s->entries[i];
        uint32_t age = now_ms - s->entries[i].last_confirmed_ms;
        if (!victim || age > victim_age) {
            victim = &s->entries[i];
            victim_age = age;
        }
    }
    return victim;
}

identity_pin_result_t identity_store_pin(identity_store_t* s, uint32_t address,
                                         const uint8_t ed25519_pub[32],
                                         const uint8_t x25519_pub[32], uint32_t now_ms) {
    identity_pin_t* e = find_entry(s, address);
    if (e) {
        if (memcmp(e->ed25519_pub, ed25519_pub, 32) == 0 &&
            memcmp(e->x25519_pub, x25519_pub, 32) == 0) {
            /* Identical re-attestation: refresh the LRU position only.
             * Attestations are replayed on a cadence by design; re-pinning
             * must not churn the entry. */
            e->last_confirmed_ms = now_ms;
            return IDENTITY_PIN_REFRESHED;
        }
        /* CONFLICT: first-seen wins (TOFU). Reject the new binding and do
         * not touch the entry at all: a conflicting frame is not a
         * confirmation, so it must not move the LRU position either. This
         * is the impersonation-detection payoff: a keyed insider attesting
         * this address under different keys is detected and refused. */
        s->conflicts++;
        return IDENTITY_PIN_CONFLICT;
    }

    e = alloc_entry(s, now_ms);
    e->used = true;
    e->address = address;
    memcpy(e->ed25519_pub, ed25519_pub, 32);
    memcpy(e->x25519_pub, x25519_pub, 32);
    e->pinned_at_ms = now_ms;
    e->last_confirmed_ms = now_ms;
    return IDENTITY_PIN_NEW;
}

identity_pin_result_t identity_store_handle_attestation(identity_store_t* s,
                                                        const bramble_identity_attestation_t* att,
                                                        uint32_t self_addr, uint32_t now_ms,
                                                        uint64_t epoch_ms) {
    /* Our own attestation echoed back through the flood: nothing to pin
     * (we ARE the binding). Also covers the impersonation-of-self case:
     * an insider attesting OUR address never touches our store; the rest
     * of the mesh detects the conflict against our genuine binding. */
    if (att->src_addr == self_addr)
        return IDENTITY_PIN_SELF;

    /* Phase 4 address<->key binding, checked BEFORE the expensive Ed25519
     * verify (one SHA256): the claimed src_addr must BE the address the
     * frame's own Ed25519 key derives to. Without this, any keyed insider
     * could attest any address under its own (validly signing) key and
     * win wherever it was heard first; with it, claiming an address means
     * holding a key whose SHA256[0:4] equals it, i.e. a preimage search.
     * Counted separately: a delivered (MAC-valid) mismatch is a keyed
     * member misbehaving, not line noise. */
    if (crypto_derive_address(att->ed25519_pub) != att->src_addr) {
        s->addr_mismatches++;
        return IDENTITY_PIN_ADDR_MISMATCH;
    }

    /* The ONLY receive-side Ed25519 verify (relays never run this): the
     * canonical message is rebuilt from the frame's own fields and checked
     * against the frame's embedded ed25519_pub. A failure here on a
     * DELIVERED frame means a network-key holder sent garbage (the relay
     * gate already proved key possession), hence the counter: it is an
     * insider-misbehavior signal, not line noise. */
    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    if (bramble_identity_attestation_signed_msg(att, msg, sizeof(msg)) != ESP_OK ||
        !crypto_ed25519_verify(att->ed25519_pub, msg, sizeof(msg), att->sig)) {
        s->sig_failures++;
        return IDENTITY_PIN_BAD_SIG;
    }

    /* Trust-anchor endorsement gate (P2): runs ONLY on an anchored node, and
     * ONLY after the existing self/addr/self-sig checks above (so BAD_SIG and
     * ADDR_MISMATCH still take precedence: a frame that is both unendorsed and
     * addr-mismatched returns ADDR_MISMATCH). A node with no anchor skips this
     * block entirely and its behavior is today's TOFU, bit-for-bit: the opt-in
     * guarantee. This gates PINNING only; the caller still relays the frame. */
    if (s->has_anchor) {
        /* (a) cert present: not_after == 0 is the "no cert" sentinel (the
         * endorsement_sig is then all-zero). An anchored node refuses to pin
         * an unendorsed identity. */
        if (att->not_after == IDENTITY_ENDORSEMENT_NOT_AFTER_NONE) {
            s->unendorsed++;
            return IDENTITY_PIN_UNENDORSED;
        }
        /* (b) the anchor's signature must vouch for THIS frame's ed25519_pub
         * under THIS not_after. Rejects a wrong-anchor cert and a cert minted
         * for a different node's key (cross-node graft): the endorsement
         * message binds the exact ed25519_pub. */
        if (!identity_endorsement_verify(s->anchor_pub, att->ed25519_pub, att->not_after,
                                         att->endorsement_sig)) {
            s->unendorsed++;
            return IDENTITY_PIN_UNENDORSED;
        }
        /* (c) expiry: a non-permanent cert is expired once the SYNCED wall
         * clock is past its not_after. v1 always issues UINT64_MAX (permanent)
         * so this never fires live, but the wire format is frozen now, so it
         * is implemented and tested for P-future expiring certs. epoch_ms == 0
         * means the clock is unsynced: expiry is NOT enforced (a would-be
         * expiring cert is provisionally accepted until the clock syncs,
         * matching the design's "expiry only after sync" resolution; a
         * permanent cert is unaffected regardless). */
        if (att->not_after != IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT && epoch_ms != 0 &&
            epoch_ms > att->not_after) {
            s->expired++;
            return IDENTITY_PIN_EXPIRED;
        }
    }

    return identity_store_pin(s, att->src_addr, att->ed25519_pub, att->x25519_pub, now_ms);
}

const identity_pin_t* identity_store_lookup(const identity_store_t* s, uint32_t address) {
    for (int i = 0; i < IDENTITY_STORE_CAPACITY; i++) {
        if (s->entries[i].used && s->entries[i].address == address)
            return &s->entries[i];
    }
    return NULL;
}

bool identity_store_quorum_eligible(const identity_store_t* s, uint32_t address, bool established,
                                    uint32_t now_ms) {
    if (!established)
        return false; /* tenure requirement is never relaxed */
    if (identity_store_lookup(s, address) != NULL)
        return true; /* pinned: always eligible (subject to tenure) */
    /* Unpinned: eligible ONLY inside the bounded per-boot grace, and P3a
     * tightens that fallback with the count early-exit so the residual
     * unpinned-trust window is as small as possible:
     *   (1) within QUORUM_BOOTSTRAP_GRACE_MS of boot -- the time backstop, so a
     *       fresh mesh can bootstrap timesync before any attestation is pinned;
     *   (2) FEWER than QUORUM_GRACE_MIN_PINS pins held -- early-exit: once the
     *       node has real corroboration from pinned peers the unpinned fallback
     *       is no longer needed, so the gate tightens to pinned-only the instant
     *       enough pins exist, typically well before the time bound. This is
     *       anchor-INDEPENDENT (it applies to no-anchor TOFU meshes too, a
     *       deliberate strict improvement to the #131 grace: see the header).
     * The subtraction is uint32 wraparound-safe, the same now_ms idiom used for
     * LRU age above. (A negative-intel reject ring was considered and dropped;
     * see QUORUM_GRACE_MIN_PINS for why.) */
    if (s->grace_forced_closed)
        return false; /* a runtime re-hardening (anchor change that dropped
                       * pins) closes the unpinned fallback permanently: a node
                       * that already had pins wiped must corroborate from
                       * ENDORSED pins only, never re-open the unpinned window
                       * just because its count reset to 0 inside the grace. */
    if ((uint32_t)(now_ms - s->boot_ms) < QUORUM_BOOTSTRAP_GRACE_MS &&
        identity_store_count(s) < (int)QUORUM_GRACE_MIN_PINS)
        return true;
    /* After the grace or once enough pins corroborate: an unpinned peer NEVER
     * corroborates. */
    return false;
}

int identity_store_count(const identity_store_t* s) {
    int n = 0;
    for (int i = 0; i < IDENTITY_STORE_CAPACITY; i++) {
        if (s->entries[i].used)
            n++;
    }
    return n;
}
