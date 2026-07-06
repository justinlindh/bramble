#include "include/dm_session.h"
#include <string.h>
#include <stdio.h>

/*
 * Constant-time big-endian lexicographic "a <= b" over two 32-byte SECRET
 * DH outputs. Every byte pair is processed unconditionally (no early exit,
 * no branch whose target depends on where a and b first differ), unlike
 * memcmp, which typically stops scanning at the first differing byte and
 * so leaks that byte's position through timing. Returns 1 if a <= b, 0
 * otherwise; ties (a == b) count as <=. The only property agreement needs
 * is that both parties apply the identical deterministic total order, which
 * this does regardless of timing.
 */
static int ct_le32(const uint8_t a[32], const uint8_t b[32]) {
    uint32_t decided = 0; /* 1 once a more-significant byte already differed */
    uint32_t a_le_b = 1;  /* running answer; equal-so-far counts as <= */
    for (int i = 0; i < 32; i++) {
        uint32_t ai = a[i], bi = b[i];
        uint32_t is_gt = ((uint32_t)(bi - ai)) >> 31; /* 1 iff ai > bi */
        uint32_t is_lt = ((uint32_t)(ai - bi)) >> 31; /* 1 iff ai < bi */
        uint32_t is_diff = is_gt | is_lt;
        uint32_t apply = is_diff & (1u - decided);
        uint32_t apply_mask = 0u - apply; /* all-ones if apply else 0 */
        a_le_b = (a_le_b & ~apply_mask) | (is_lt & apply_mask);
        decided = decided | is_diff;
    }
    return (int)a_le_b;
}

/*
 * IKM = DH1(eph,eph) || DH2(id,id) || sorted{DH3(my_eph,peer_id), DH4(my_id,peer_eph)}.
 * RFC section 1 (2026-06-12-crypto-design-r2.md) states the formula as
 * DH1 || DH2 || DH3 || DH4 in "my/peer" terms, without addressing that DH3
 * and DH4 swap between the two parties: X25519 is symmetric, so
 * X25519(a_eph_priv, b_id_pub) == X25519(b_id_priv, a_eph_pub), i.e. party
 * A's DH3 equals party B's DH4, and A's DH4 equals B's DH3. Concatenating
 * "my DH3 then my DH4" per party therefore gives A and B the SAME two
 * 32-byte values but in SWAPPED order, so a naive implementation derives
 * different IKM (and thus different session keys) on each side.
 *
 * Fix: both parties compute the same UNORDERED pair {DH3, DH4} (one party's
 * DH3 is the other's DH4 and vice versa), so sorting that pair by raw byte
 * value before concatenating yields an identical ordering on both sides
 * regardless of which party is "A" or "B". The compare and the placement
 * are both constant-time: cross_a/cross_b are secret DH outputs, so neither
 * the comparison nor which one lands in which IKM slot may depend on secret
 * data through a timing-observable branch. DH1 and DH2 need no such fix:
 * X25519(a_eph_priv,b_eph_pub) == X25519(b_eph_priv,a_eph_pub) and likewise
 * for DH2, so both parties already compute the same value in the same slot
 * without any role-dependent naming.
 */
static int dm_compute_ikm(const uint8_t my_id_priv[32], const uint8_t my_eph_priv[32],
                          const uint8_t peer_id_pub[32], const uint8_t peer_eph_pub[32],
                          uint8_t ikm_out[128]) {
    if (crypto_x25519_dh(my_eph_priv, peer_eph_pub, ikm_out + 0) != 0)
        return -1;
    if (crypto_x25519_dh(my_id_priv, peer_id_pub, ikm_out + 32) != 0)
        return -1;

    uint8_t cross_a[32]; /* my_eph x peer_id */
    uint8_t cross_b[32]; /* my_id  x peer_eph */
    if (crypto_x25519_dh(my_eph_priv, peer_id_pub, cross_a) != 0)
        return -1;
    if (crypto_x25519_dh(my_id_priv, peer_eph_pub, cross_b) != 0)
        return -1;

    /* Canonical ordering: min-first, both compare and placement branchless. */
    uint32_t le_mask = 0u - (uint32_t)ct_le32(cross_a, cross_b); /* all-ones if cross_a<=cross_b */
    for (int i = 0; i < 32; i++) {
        ikm_out[64 + i] = (uint8_t)((cross_a[i] & le_mask) | (cross_b[i] & ~le_mask));
        ikm_out[96 + i] = (uint8_t)((cross_b[i] & le_mask) | (cross_a[i] & ~le_mask));
    }
    return 0;
}

/* Shared by dm_derive_session_key, the K_ke_init tag, and the K_confirm
 * tag: lo(4 BE) || hi(4 BE) || ke_epoch(2 LE), lo/hi = min/max(addr_a,
 * addr_b) so the result is identical regardless of which side calls it
 * with its own address first. */
static void dm_build_info(uint32_t addr_a, uint32_t addr_b, uint16_t ke_epoch, uint8_t info[10]) {
    uint32_t lo = addr_a < addr_b ? addr_a : addr_b;
    uint32_t hi = addr_a < addr_b ? addr_b : addr_a;
    info[0] = (uint8_t)(lo >> 24);
    info[1] = (uint8_t)(lo >> 16);
    info[2] = (uint8_t)(lo >> 8);
    info[3] = (uint8_t)lo;
    info[4] = (uint8_t)(hi >> 24);
    info[5] = (uint8_t)(hi >> 16);
    info[6] = (uint8_t)(hi >> 8);
    info[7] = (uint8_t)hi;
    info[8] = (uint8_t)(ke_epoch & 0xFF);
    info[9] = (uint8_t)((ke_epoch >> 8) & 0xFF);
}

/* Derives the session key from an already-computed 128-byte IKM. Split out
 * of dm_derive_session_key so dm_build_resp/dm_verify_resp (Task 1.3) can
 * compute the IKM once via dm_compute_ikm and reuse it both here and for
 * K_confirm, instead of repeating the four X25519 scalar multiplications
 * (each 30-100ms on the S3 per the RFC's compute-placement note). */
static int dm_session_key_from_ikm(const uint8_t ikm[128], uint32_t addr_a, uint32_t addr_b,
                                   uint16_t ke_epoch, uint8_t session_key_out[32]) {
    uint8_t info[10];
    dm_build_info(addr_a, addr_b, ke_epoch, info);
    const char* salt = "bramble-dm-v2";
    return crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), ikm, 128, info, sizeof(info),
                              session_key_out, 32);
}

int dm_derive_session_key(const uint8_t my_id_priv[32], const uint8_t my_eph_priv[32],
                          const uint8_t peer_id_pub[32], const uint8_t peer_eph_pub[32],
                          uint32_t addr_a, uint32_t addr_b, uint16_t ke_epoch,
                          uint8_t session_key_out[32]) {
    uint8_t ikm[128];
    if (dm_compute_ikm(my_id_priv, my_eph_priv, peer_id_pub, peer_eph_pub, ikm) != 0)
        return -1;
    return dm_session_key_from_ikm(ikm, addr_a, addr_b, ke_epoch, session_key_out);
}

int dm_derive_sas(const uint8_t session_ikm[128], char sas_out[8]) {
    const char* salt = "bramble-sas";
    uint8_t okm[4];
    if (crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), session_ikm, 128, NULL, 0, okm,
                           sizeof(okm)) != 0)
        return -1;
    /* Render as 7 decimal digits, human-comparable. */
    uint32_t v =
        ((uint32_t)okm[0] << 24) | ((uint32_t)okm[1] << 16) | ((uint32_t)okm[2] << 8) | okm[3];
    snprintf(sas_out, 8, "%07u", (unsigned)(v % 10000000u));
    return 0;
}

/*
 * Constant-time equality over two byte buffers (tag comparison). OR-
 * accumulate every byte's XOR difference with no early exit, so comparing
 * a computed tag against an attacker-supplied one never leaks how many
 * leading bytes matched through timing. Used for EVERY tag check below;
 * never memcmp on a tag.
 */
static int ct_eq(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t r = 0;
    for (size_t i = 0; i < n; i++)
        r |= a[i] ^ b[i];
    return r == 0;
}

/*
 * Derives a handshake tag: HKDF(salt="bramble-dm-v2", ikm, label || info)
 * used as an HMAC key over transcript, truncated to 16 bytes. Shared by
 * both the K_ke_init/transcript_1 tag (dm_build_init/dm_verify_init) and
 * the K_confirm/transcript_2 tag (dm_build_resp/dm_verify_resp), so the
 * label length is ALWAYS computed with strlen(label) here, in exactly one
 * place, rather than a hardcoded byte count at each of the four call
 * sites. A hardcoded length is a footgun: "bramble-ke-init" is 15 bytes
 * (no NUL) and "bramble-ke-confirm" is 18 (no NUL), and a future edit that
 * changed one length without updating its matching site elsewhere would
 * silently break every DM session (the build and verify sides would
 * derive different keys with no compile-time or obvious runtime signal).
 * Deriving it from strlen() at a single call site removes the possibility
 * entirely.
 */
static int dm_derive_ke_tag(const char* label, const uint8_t* ikm, size_t ikm_len, uint32_t addr_a,
                            uint32_t addr_b, uint16_t ke_epoch, const uint8_t* transcript,
                            size_t transcript_len, uint8_t tag_out[16]) {
    size_t label_len = strlen(label);
    uint8_t info[10];
    dm_build_info(addr_a, addr_b, ke_epoch, info);

    uint8_t hkdf_info[32]; /* longest label (18) + 10-byte info = 28, generous */
    if (label_len + sizeof(info) > sizeof(hkdf_info))
        return -1; /* defensive; never trips today */
    memcpy(hkdf_info, label, label_len);
    memcpy(hkdf_info + label_len, info, sizeof(info));

    uint8_t key[32];
    const char* salt = "bramble-dm-v2";
    if (crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), ikm, ikm_len, hkdf_info,
                           label_len + sizeof(info), key, sizeof(key)) != 0)
        return -1;

    uint8_t full_mac[32];
    if (crypto_hmac_sha256(key, sizeof(key), transcript, transcript_len, full_mac) != 0)
        return -1;
    memcpy(tag_out, full_mac, 16);
    return 0;
}

int dm_build_init(const bramble_identity_t* my_id, const uint8_t my_eph_pub[32],
                  const uint8_t my_eph_priv[32], uint32_t peer_addr, uint16_t ke_epoch,
                  const uint8_t* peer_id_pub_or_null, bramble_key_exchange_t* out) {
    memset(out, 0, sizeof(*out));
    out->src_addr = my_id->address;
    memcpy(out->ephemeral_pubkey, my_eph_pub, 32);
    memcpy(out->long_term_pubkey, my_id->public_key, 32);
    out->key_id = (uint8_t)(ke_epoch & 0xFF);
    out->ke_type = KE_TYPE_INIT;

    if (!peer_id_pub_or_null) {
        /* First contact: no peer-keyed DH is computable yet (the
         * initiator doesn't hold peer_id_pub). Zero tag, stated not
         * hidden: proves nothing, the initiator is authenticated later
         * (message 3, the first DATA under the session key). */
        memset(out->auth_tag, 0, sizeof(out->auth_tag));
        return 0;
    }

    /* Rekey / known peer (RFC section 1's B1 construction). DH2 is
     * static-static (symmetric, same value either side computes it from).
     * DH3 is the initiator's OWN ephemeral bound to the peer's identity;
     * only the initiator's ephemeral exists at this point (the responder
     * hasn't generated one yet), so this is a fixed, role-specific
     * pairing, not Task 1.1's role-agnostic sorted cross-term (that sort
     * exists only because the FINAL session key mixes both parties'
     * ephemerals symmetrically after both exist; here only one does). */
    uint8_t dh2[32], dh3[32];
    if (crypto_x25519_dh(my_id->private_key, peer_id_pub_or_null, dh2) != 0)
        return -1;
    if (crypto_x25519_dh(my_eph_priv, peer_id_pub_or_null, dh3) != 0)
        return -1;

    uint8_t ikm[64];
    memcpy(ikm, dh2, 32);
    memcpy(ikm + 32, dh3, 32);

    /* transcript_1 = addr_init || addr_resp || eph_init || id_init */
    uint8_t transcript[4 + 4 + 32 + 32];
    transcript[0] = (uint8_t)(my_id->address >> 24);
    transcript[1] = (uint8_t)(my_id->address >> 16);
    transcript[2] = (uint8_t)(my_id->address >> 8);
    transcript[3] = (uint8_t)my_id->address;
    transcript[4] = (uint8_t)(peer_addr >> 24);
    transcript[5] = (uint8_t)(peer_addr >> 16);
    transcript[6] = (uint8_t)(peer_addr >> 8);
    transcript[7] = (uint8_t)peer_addr;
    memcpy(transcript + 8, my_eph_pub, 32);
    memcpy(transcript + 40, my_id->public_key, 32);

    return dm_derive_ke_tag("bramble-ke-init", ikm, sizeof(ikm), my_id->address, peer_addr,
                            ke_epoch, transcript, sizeof(transcript), out->auth_tag);
}

int dm_verify_init(const bramble_key_exchange_t* msg, const bramble_identity_t* my_id,
                   int have_peer_id, const uint8_t peer_id_pub[32],
                   const uint8_t* pinned_peer_x25519_or_null) {
    /* Dispatch-confusion guard: without this, a message built as RESP
     * (or any other type) but presented to dm_verify_init would, on a
     * first-contact call (have_peer_id==0, no tag to check at all), be
     * accepted outright as if it were a valid INIT. Domain-separated HKDF
     * labels already prevent tag forgery across message types, but
     * asserting ke_type closes the dispatch confusion itself rather than
     * relying on that as the only defense. */
    if (msg->ke_type != KE_TYPE_INIT)
        return -1;

    /* Phase 4 pin continuity (replaces the pre-rebind
     * derive_address(long_term_pubkey) == src_addr binding, which cannot
     * hold now that the address derives from the Ed25519 identity key):
     * when the caller holds an attestation-verified pin for src_addr, the
     * handshake's X25519 identity key MUST be the pinned one. A public-key
     * compare, not a secret: memcmp is fine. */
    if (pinned_peer_x25519_or_null &&
        memcmp(msg->long_term_pubkey, pinned_peer_x25519_or_null, 32) != 0)
        return DM_VERIFY_ERR_PIN_MISMATCH;

    if (!have_peer_id) {
        /* First contact: no tag to check by design. */
        return 0;
    }

    uint32_t addr_init = msg->src_addr;
    uint32_t addr_resp = my_id->address;
    uint16_t ke_epoch = (uint16_t)msg->key_id;

    uint8_t dh2[32], dh3[32];
    if (crypto_x25519_dh(my_id->private_key, peer_id_pub, dh2) != 0)
        return -1;
    /* My identity bound to the initiator's ephemeral (msg->ephemeral_pubkey).
     * By X25519 symmetry this equals the initiator's own
     * X25519(their eph_priv, my id_pub) computed in dm_build_init; the
     * verifier needs no ephemeral of its own for this term. */
    if (crypto_x25519_dh(my_id->private_key, msg->ephemeral_pubkey, dh3) != 0)
        return -1;

    uint8_t ikm[64];
    memcpy(ikm, dh2, 32);
    memcpy(ikm + 32, dh3, 32);

    uint8_t transcript[4 + 4 + 32 + 32];
    transcript[0] = (uint8_t)(addr_init >> 24);
    transcript[1] = (uint8_t)(addr_init >> 16);
    transcript[2] = (uint8_t)(addr_init >> 8);
    transcript[3] = (uint8_t)addr_init;
    transcript[4] = (uint8_t)(addr_resp >> 24);
    transcript[5] = (uint8_t)(addr_resp >> 16);
    transcript[6] = (uint8_t)(addr_resp >> 8);
    transcript[7] = (uint8_t)addr_resp;
    memcpy(transcript + 8, msg->ephemeral_pubkey, 32);
    memcpy(transcript + 40, msg->long_term_pubkey, 32);

    uint8_t expect_tag[16];
    if (dm_derive_ke_tag("bramble-ke-init", ikm, sizeof(ikm), addr_init, addr_resp, ke_epoch,
                         transcript, sizeof(transcript), expect_tag) != 0)
        return -1;

    return ct_eq(expect_tag, msg->auth_tag, 16) ? 0 : -1;
}

int dm_build_resp(const bramble_identity_t* my_id, const uint8_t my_eph_pub[32],
                  const uint8_t my_eph_priv[32], const bramble_key_exchange_t* init,
                  uint16_t ke_epoch, bramble_key_exchange_t* out, uint8_t session_key_out[32]) {
    memset(out, 0, sizeof(*out));
    out->src_addr = my_id->address;
    memcpy(out->ephemeral_pubkey, my_eph_pub, 32);
    memcpy(out->long_term_pubkey, my_id->public_key, 32);
    out->key_id = (uint8_t)(ke_epoch & 0xFF);
    out->ke_type = KE_TYPE_RESP;

    uint8_t ikm[128];
    if (dm_compute_ikm(my_id->private_key, my_eph_priv, init->long_term_pubkey,
                       init->ephemeral_pubkey, ikm) != 0)
        return -1;
    if (dm_session_key_from_ikm(ikm, my_id->address, init->src_addr, ke_epoch, session_key_out) !=
        0)
        return -1;

    /* transcript_2 = addr_init || addr_resp || eph_init || id_init ||
     * eph_resp || id_resp: the RFC states "over the full transcript, both
     * ephemerals and both identities now known" without an exact byte
     * layout; this extends transcript_1's exact field order with the two
     * newly-known responder fields, binding every field an attacker could
     * swap or reflect (unknown-key-share / reflection defense). */
    uint8_t transcript[4 + 4 + 32 + 32 + 32 + 32];
    transcript[0] = (uint8_t)(init->src_addr >> 24);
    transcript[1] = (uint8_t)(init->src_addr >> 16);
    transcript[2] = (uint8_t)(init->src_addr >> 8);
    transcript[3] = (uint8_t)init->src_addr;
    transcript[4] = (uint8_t)(my_id->address >> 24);
    transcript[5] = (uint8_t)(my_id->address >> 16);
    transcript[6] = (uint8_t)(my_id->address >> 8);
    transcript[7] = (uint8_t)my_id->address;
    memcpy(transcript + 8, init->ephemeral_pubkey, 32);
    memcpy(transcript + 40, init->long_term_pubkey, 32);
    memcpy(transcript + 72, my_eph_pub, 32);
    memcpy(transcript + 104, my_id->public_key, 32);

    return dm_derive_ke_tag("bramble-ke-confirm", ikm, sizeof(ikm), init->src_addr, my_id->address,
                            ke_epoch, transcript, sizeof(transcript), out->auth_tag);
}

int dm_verify_resp(const bramble_key_exchange_t* resp, const bramble_identity_t* my_id,
                   const uint8_t my_eph_priv[32], const uint8_t my_eph_pub[32], uint16_t ke_epoch,
                   const uint8_t* pinned_peer_x25519_or_null, uint8_t session_key_out[32]) {
    /* Dispatch-confusion guard, same rationale as dm_verify_init: reject a
     * message that isn't actually a RESP before doing anything else with
     * it. */
    if (resp->ke_type != KE_TYPE_RESP)
        return -1;

    /* Phase 4 pin continuity, same semantics as dm_verify_init. src_addr
     * tampering is caught below by the K_confirm tag (transcript_2 binds
     * both addresses); this check is about a KNOWN peer showing up with a
     * DIFFERENT X25519 key than its attested, pinned one. */
    if (pinned_peer_x25519_or_null &&
        memcmp(resp->long_term_pubkey, pinned_peer_x25519_or_null, 32) != 0)
        return DM_VERIFY_ERR_PIN_MISMATCH;

    uint8_t ikm[128];
    if (dm_compute_ikm(my_id->private_key, my_eph_priv, resp->long_term_pubkey,
                       resp->ephemeral_pubkey, ikm) != 0)
        return -1;

    /* Compute into a local buffer, not the caller's session_key_out, until
     * the confirm tag verifies: a caller that ignores the return value
     * must never observe an unauthenticated key. No secrecy is at stake
     * either way (this is the verifier's own computation from its own
     * private key), but a discipline of "the output is only valid on
     * success" is worth keeping regardless. */
    uint8_t local_key[32];
    if (dm_session_key_from_ikm(ikm, my_id->address, resp->src_addr, ke_epoch, local_key) != 0)
        return -1;

    uint8_t transcript[4 + 4 + 32 + 32 + 32 + 32];
    transcript[0] = (uint8_t)(my_id->address >> 24);
    transcript[1] = (uint8_t)(my_id->address >> 16);
    transcript[2] = (uint8_t)(my_id->address >> 8);
    transcript[3] = (uint8_t)my_id->address;
    transcript[4] = (uint8_t)(resp->src_addr >> 24);
    transcript[5] = (uint8_t)(resp->src_addr >> 16);
    transcript[6] = (uint8_t)(resp->src_addr >> 8);
    transcript[7] = (uint8_t)resp->src_addr;
    memcpy(transcript + 8, my_eph_pub, 32);
    memcpy(transcript + 40, my_id->public_key, 32);
    memcpy(transcript + 72, resp->ephemeral_pubkey, 32);
    memcpy(transcript + 104, resp->long_term_pubkey, 32);

    uint8_t expect_tag[16];
    if (dm_derive_ke_tag("bramble-ke-confirm", ikm, sizeof(ikm), my_id->address, resp->src_addr,
                         ke_epoch, transcript, sizeof(transcript), expect_tag) != 0)
        return -1;

    if (!ct_eq(expect_tag, resp->auth_tag, 16))
        return -1;

    memcpy(session_key_out, local_key, 32);
    return 0;
}

void dm_table_init(dm_table_t* t) { memset(t, 0, sizeof(*t)); }

dm_session_t* dm_lookup(dm_table_t* t, uint32_t peer_addr) {
    for (int i = 0; i < DM_MAX_SESSIONS; i++) {
        if (t->s[i].state != DM_STATE_NONE && t->s[i].peer_addr == peer_addr) {
            return &t->s[i];
        }
    }
    return NULL;
}

bool dm_session_teardown(dm_table_t* t, uint32_t peer_addr) {
    dm_session_t* s = dm_lookup(t, peer_addr);
    if (!s)
        return false;
    memset(s, 0, sizeof(*s)); /* state -> DM_STATE_NONE, key + peer_id_pub wiped */
    return true;
}

bool dm_pin_disagrees(const dm_session_t* s, const uint8_t pinned_x25519[32]) {
    /* Only an ESTABLISHED session is authoritative to compare against; a
     * public-key compare (peer_id_pub is not secret), so memcmp is fine. */
    return s->state == DM_STATE_ACTIVE && memcmp(s->peer_id_pub, pinned_x25519, 32) != 0;
}

dm_session_t* dm_alloc(dm_table_t* t, uint32_t peer_addr, uint32_t now_ms) {
    /* Reuse an existing slot for this peer: not a new handshake, so no cap
     * check. The slot's existing state already counts toward the cap if it
     * is DM_STATE_HANDSHAKING; returning it again doesn't add a new one. */
    dm_session_t* existing = dm_lookup(t, peer_addr);
    if (existing)
        return existing;

    /* Handshaking cap (M4 DoS defense): every fresh slot (free or evicted)
     * is a prospective handshake, since callers transition it to
     * DM_STATE_HANDSHAKING immediately after a successful call. Refuse to
     * create one at all, even if an evictable slot exists, once the cap is
     * already reached: a full table under the cap must never be raided to
     * start a 9th handshake. */
    int handshaking_count = 0;
    for (int i = 0; i < DM_MAX_SESSIONS; i++) {
        if (t->s[i].state == DM_STATE_HANDSHAKING)
            handshaking_count++;
    }
    if (handshaking_count >= DM_MAX_HANDSHAKING)
        return NULL;

    /* Free (DM_STATE_NONE) slot. */
    for (int i = 0; i < DM_MAX_SESSIONS; i++) {
        if (t->s[i].state == DM_STATE_NONE) {
            memset(&t->s[i], 0, sizeof(t->s[i]));
            t->s[i].peer_addr = peer_addr;
            t->s[i].established_ms = now_ms;
            t->s[i].last_active_ms = now_ms;
            return &t->s[i];
        }
    }

    /* LRU-evict the slot with the smallest last_active_ms that is NOT
     * VERIFIED ACTIVE. Fix 1 (red-team panel): a first-contact INIT needs
     * no secret and lands directly in ACTIVE/verified=0, so protecting
     * every ACTIVE slot regardless of verified let an attacker fill the
     * whole table with permanently-unevictable forged sessions. Only
     * state==ACTIVE && verified==1 is fully protected here; HANDSHAKING
     * and UNVERIFIED ACTIVE (state==ACTIVE && verified==0) share the same
     * evictable pool, ordered by last_active_ms so a genuinely-active
     * unverified session still outlives an idle forged one. */
    int victim = -1;
    for (int i = 0; i < DM_MAX_SESSIONS; i++) {
        if (t->s[i].state == DM_STATE_ACTIVE && t->s[i].verified)
            continue; /* VERIFIED ACTIVE: fully protected */
        if (victim < 0 || t->s[i].last_active_ms < t->s[victim].last_active_ms) {
            victim = i;
        }
    }
    if (victim < 0)
        return NULL; /* table full, nothing evictable */

    memset(&t->s[victim], 0, sizeof(t->s[victim]));
    t->s[victim].peer_addr = peer_addr;
    t->s[victim].established_ms = now_ms;
    t->s[victim].last_active_ms = now_ms;
    return &t->s[victim];
}

int dm_session_encrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr,
                       const uint8_t* pt, size_t pt_len, const uint8_t nonce[12], uint8_t* ct_out,
                       uint8_t* tag_out) {
    uint8_t aad[HEADER_SIZE + 4];
    if (bramble_build_aead_aad(h, src_addr, aad, sizeof(aad)) != ESP_OK)
        return -1;
    return crypto_aes256gcm_encrypt(s->session_key, nonce, pt, pt_len, aad, sizeof(aad), ct_out,
                                    tag_out);
}

int dm_session_decrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr,
                       const uint8_t nonce[12], const uint8_t* ct, size_t ct_len,
                       const uint8_t* tag, uint8_t* pt_out) {
    uint8_t aad[HEADER_SIZE + 4];
    if (bramble_build_aead_aad(h, src_addr, aad, sizeof(aad)) != ESP_OK)
        return -1;
    return crypto_aes256gcm_decrypt(s->session_key, nonce, ct, ct_len, aad, sizeof(aad), tag,
                                    pt_out);
}
