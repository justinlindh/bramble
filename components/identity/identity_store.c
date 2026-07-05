/*
 * Verified TOFU identity pin store (per-node identity Phase 3, Part C).
 * See identity_store.h for the trust model. Pure C over a caller-owned
 * struct: host-testable directly, one instance per node in gosim, one
 * static instance in mesh_task.c.
 */
#include "include/identity_store.h"

#include <string.h>

#include "crypto.h" /* crypto_ed25519_verify */

void identity_store_init(identity_store_t* s) { memset(s, 0, sizeof(*s)); }

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
                                                        uint32_t self_addr, uint32_t now_ms) {
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

    return identity_store_pin(s, att->src_addr, att->ed25519_pub, att->x25519_pub, now_ms);
}

const identity_pin_t* identity_store_lookup(const identity_store_t* s, uint32_t address) {
    for (int i = 0; i < IDENTITY_STORE_CAPACITY; i++) {
        if (s->entries[i].used && s->entries[i].address == address)
            return &s->entries[i];
    }
    return NULL;
}

bool identity_store_quorum_eligible(const identity_store_t* s, uint32_t address, bool established) {
    if (!established)
        return false; /* tenure requirement is never relaxed */
    if (identity_store_count(s) == 0)
        return true; /* fresh mesh / fresh boot: fall back to tenure only */
    return identity_store_lookup(s, address) != NULL;
}

int identity_store_count(const identity_store_t* s) {
    int n = 0;
    for (int i = 0; i < IDENTITY_STORE_CAPACITY; i++) {
        if (s->entries[i].used)
            n++;
    }
    return n;
}
