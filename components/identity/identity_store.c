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
     * not just future attestations. boot_ms and the counters are NOT reset: the
     * bootstrap grace stays expired (an anchored, freshly-cleared store must
     * corroborate timesync only from re-pinned ENDORSED peers, never re-open the
     * unpinned-trust window; the fleet re-attests on the boot+15min cadence),
     * and the diagnostic counters are cumulative. Idempotent same-key re-set
     * leaves the endorsed pins in place. */
    if (changed) {
        for (int i = 0; i < IDENTITY_STORE_CAPACITY; i++)
            s->entries[i].used = false;
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

/* Push a rejected address into the negative-intel FIFO (trust-anchor P3a).
 * De-duplicates against the WHOLE ring, not just the head: a Sybil replaying
 * its rejected attestation on the boot+15min cadence (or two Sybils
 * alternating) must not collapse the 16-slot ring to one or two addresses and
 * evict other nodes' negative intel. A 16-entry linear scan per rejection is
 * negligible and only runs on the rejection path. */
static void reject_ring_push(identity_store_t* s, uint32_t address) {
    if (identity_store_addr_rejected(s, address))
        return;
    s->reject_ring[s->reject_ring_head] = address;
    s->reject_ring_head = (uint8_t)((s->reject_ring_head + 1u) % IDENTITY_REJECT_RING);
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
            /* Negative intel (P3a): we have now SEEN this address attest and
             * reveal itself as not-endorsed. Record it so it can never ride the
             * bootstrap grace as an established-but-unpinned quorum source. Only
             * the endorsement verdicts (UNENDORSED/EXPIRED) are recorded; the
             * pre-endorsement failures (BAD_SIG/ADDR_MISMATCH) above are not,
             * since they are not evidence about endorsement status. */
            reject_ring_push(s, att->src_addr);
            return IDENTITY_PIN_UNENDORSED;
        }
        /* (b) the anchor's signature must vouch for THIS frame's ed25519_pub
         * under THIS not_after. Rejects a wrong-anchor cert and a cert minted
         * for a different node's key (cross-node graft): the endorsement
         * message binds the exact ed25519_pub. */
        if (!identity_endorsement_verify(s->anchor_pub, att->ed25519_pub, att->not_after,
                                         att->endorsement_sig)) {
            s->unendorsed++;
            reject_ring_push(s, att->src_addr); /* negative intel (see above) */
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
            reject_ring_push(s, att->src_addr); /* negative intel (see above) */
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
     * tightens that fallback with two further conditions so the residual
     * unpinned-trust window is as small as possible:
     *   (1) within QUORUM_BOOTSTRAP_GRACE_MS of boot -- the time backstop, so a
     *       fresh mesh can bootstrap timesync before any attestation is pinned;
     *   (2) FEWER than QUORUM_GRACE_MIN_PINS pins held -- early-exit: once the
     *       node has real corroboration from pinned peers the unpinned fallback
     *       is no longer needed, so the gate tightens to pinned-only the instant
     *       enough pins exist, typically well before the time bound. This is
     *       anchor-INDEPENDENT (it applies to no-anchor TOFU meshes too, a
     *       deliberate strict improvement to the #131 grace: see the header);
     *   (3) this address is NOT in the reject ring -- negative intel: a
     *       self-revealing Sybil that attested and was refused for a failed
     *       endorsement is known not-endorsed and never rides the grace. (A
     *       SILENT Sybil that never attests is not in the ring and can still
     *       ride the bounded grace: an inherent, documented residual.)
     * The subtraction is uint32 wraparound-safe, the same now_ms idiom used for
     * LRU age above. */
    if ((uint32_t)(now_ms - s->boot_ms) < QUORUM_BOOTSTRAP_GRACE_MS &&
        identity_store_count(s) < (int)QUORUM_GRACE_MIN_PINS &&
        !identity_store_addr_rejected(s, address))
        return true;
    /* After the grace, once enough pins corroborate, or for a known-rejected
     * address: an unpinned peer NEVER corroborates. */
    return false;
}

bool identity_store_addr_rejected(const identity_store_t* s, uint32_t address) {
    for (int i = 0; i < IDENTITY_REJECT_RING; i++) {
        if (s->reject_ring[i] == address)
            return true;
    }
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
