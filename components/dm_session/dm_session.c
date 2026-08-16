#include "dm_session.h"
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
int dm_compute_ikm(const uint8_t my_id_priv[32], const uint8_t my_eph_priv[32],
                   const uint8_t peer_id_pub[32], const uint8_t peer_eph_pub[32],
                   uint8_t ikm_out[128]) {
    int rc = -1;
    uint8_t cross_a[32] = {0}; /* my_eph x peer_id */
    uint8_t cross_b[32] = {0}; /* my_id  x peer_eph */

    if (crypto_x25519_dh(my_eph_priv, peer_eph_pub, ikm_out + 0) != 0)
        goto out;
    if (crypto_x25519_dh(my_id_priv, peer_id_pub, ikm_out + 32) != 0)
        goto out;

    if (crypto_x25519_dh(my_eph_priv, peer_id_pub, cross_a) != 0)
        goto out;
    if (crypto_x25519_dh(my_id_priv, peer_eph_pub, cross_b) != 0)
        goto out;

    /* Canonical ordering: min-first, both compare and placement branchless. */
    uint32_t le_mask = 0u - (uint32_t)ct_le32(cross_a, cross_b); /* all-ones if cross_a<=cross_b */
    for (int i = 0; i < 32; i++) {
        ikm_out[64 + i] = (uint8_t)((cross_a[i] & le_mask) | (cross_b[i] & ~le_mask));
        ikm_out[96 + i] = (uint8_t)((cross_b[i] & le_mask) | (cross_a[i] & ~le_mask));
    }
    rc = 0;
out:
    /* Wipe the cross-term DH scratch on every exit; on failure also wipe the
     * partially-populated ikm_out (ikm_out[0:64] holds DH outputs by the time
     * any later step can fail). */
    crypto_secure_wipe(cross_a, sizeof(cross_a));
    crypto_secure_wipe(cross_b, sizeof(cross_b));
    if (rc != 0)
        crypto_secure_wipe(ikm_out, 128);
    return rc;
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

/*
 * The ratchet chains use the design's HKDF(salt=<key>, ikm="", info=<label>)
 * form (spec A.4). "ikm=empty" must still be passed as a valid non-NULL
 * pointer with length 0: the OpenSSL host wrapper's EVP_PKEY_CTX_set1_hkdf_key
 * rejects a NULL key pointer outright, and the mbedtls device path likewise
 * wants a non-NULL buffer, so a zero-length read of this byte (never actually
 * dereferenced) is the portable spelling of an empty IKM.
 */
static const uint8_t dm_ratchet_empty_ikm[1] = {0};

/* Derives the send/recv chain-key pair from a root key: two HKDF calls with
 * fixed labels, both using the same non-NULL empty IKM as every other
 * ratchet derivation above. Shared by dm_ratchet_init (RK_0) and
 * dm_session_epoch_bump (RK_{e+1}) so both paths are byte-identical. */
static int dm_derive_chain_pair(const uint8_t rk[32], uint8_t ck_lohi_out[32],
                                uint8_t ck_hilo_out[32]) {
    const char* lohi = "bramble-dm-chain-lohi";
    const char* hilo = "bramble-dm-chain-hilo";
    if (crypto_hkdf_sha256(rk, 32, dm_ratchet_empty_ikm, 0, (const uint8_t*)lohi, strlen(lohi),
                           ck_lohi_out, 32) != 0)
        return -1;
    if (crypto_hkdf_sha256(rk, 32, dm_ratchet_empty_ikm, 0, (const uint8_t*)hilo, strlen(hilo),
                           ck_hilo_out, 32) != 0)
        return -1;
    return 0;
}

int dm_ratchet_init(const uint8_t ikm[128], uint32_t addr_a, uint32_t addr_b, uint8_t rk_out[32],
                    uint8_t ck_lohi_out[32], uint8_t ck_hilo_out[32]) {
    /* RK_0 IS the legacy epoch-0 session key: migration continuity (spec A.6). */
    if (dm_session_key_from_ikm(ikm, addr_a, addr_b, 0, rk_out) != 0)
        return -1;
    return dm_derive_chain_pair(rk_out, ck_lohi_out, ck_hilo_out);
}

/*
 * Returns 0 on success, -1 if either HKDF fails. On failure BOTH outputs are
 * wiped rather than left holding whatever the caller's stack had: this used to
 * be a void function that discarded both return codes, so a failed derivation
 * handed back uninitialized stack bytes that dm_session_ratchet_encrypt then
 * used as a message key AND committed as the next chain key, permanently
 * corrupting the send chain with no way for any caller to notice.
 */
int dm_ratchet_step(const uint8_t ck_in[32], uint16_t index_n, uint8_t mk_out[32],
                    uint8_t ck_next_out[32]) {
    uint8_t mk_info[16];
    const char* mk_label = "bramble-dm-mk";
    size_t ll = strlen(mk_label);
    memcpy(mk_info, mk_label, ll);
    mk_info[ll] = (uint8_t)(index_n >> 8);
    mk_info[ll + 1] = (uint8_t)(index_n & 0xFF);
    const char* ck_label = "bramble-dm-ck";
    if (crypto_hkdf_sha256(ck_in, 32, dm_ratchet_empty_ikm, 0, mk_info, ll + 2, mk_out, 32) != 0)
        goto fail;
    if (crypto_hkdf_sha256(ck_in, 32, dm_ratchet_empty_ikm, 0, (const uint8_t*)ck_label,
                           strlen(ck_label), ck_next_out, 32) != 0)
        goto fail;
    return 0;
fail:
    /* mk_out may already hold a real message key when only the second HKDF
     * failed; wipe both so no partially derived key material survives. */
    crypto_secure_wipe(mk_out, 32);
    crypto_secure_wipe(ck_next_out, 32);
    return -1;
}

int dm_ratchet_dh(const uint8_t rk_e[32], const uint8_t dh[32], uint32_t addr_a, uint32_t addr_b,
                  uint16_t new_epoch, uint8_t rk_next_out[32]) {
    uint8_t info[10];
    dm_build_info(addr_a, addr_b, new_epoch, info);
    return crypto_hkdf_sha256(rk_e, 32, dh, 32, info, sizeof(info), rk_next_out, 32);
}

/* Installs a derived chain-key pair into a session's send/recv chains: which
 * chain goes where depends on self_is_lo (lo sends lohi, receives hilo), and
 * both chains reset to index 0 and get stamped with epoch's low byte. Shared
 * by dm_session_ratchet_init_state and dm_session_epoch_bump. */
static void dm_ratchet_install_chains(dm_ratchet_t* r, const uint8_t ck_lohi[32],
                                      const uint8_t ck_hilo[32], int self_is_lo, uint16_t epoch) {
    memcpy(r->send.ck, self_is_lo ? ck_lohi : ck_hilo, 32);
    memcpy(r->recv.ck, self_is_lo ? ck_hilo : ck_lohi, 32);
    r->send.index = 0;
    r->recv.index = 0;
    r->send.epoch = (uint8_t)(epoch & 0xFF);
    r->recv.epoch = (uint8_t)(epoch & 0xFF);
    r->send.valid = 1;
    r->recv.valid = 1;
}

/*
 * Fails closed by leaving the ratchet WIPED and both chains invalid: derivation
 * happens into locals and nothing is installed until every step has succeeded,
 * so a failure here can never produce the shape this used to have (a session
 * marked send.valid/recv.valid with zero or garbage chain keys, waved through by
 * the validity guard in dm_session_ratchet_encrypt). The caller must treat -1 as
 * "this session was never established" and refuse to mark it usable; a session
 * that silently looked valid but held garbage would encrypt undecryptable
 * frames forever, which is exactly the one-sided-desync silent message loss this
 * layer is supposed to avoid.
 */
int dm_session_ratchet_init_state(dm_session_t* s, const uint8_t ikm[128], uint32_t addr_self,
                                  uint32_t addr_peer) {
    uint32_t addr_a = addr_self < addr_peer ? addr_self : addr_peer;
    uint32_t addr_b = addr_self < addr_peer ? addr_peer : addr_self;
    uint8_t rk[32], ck_lohi[32], ck_hilo[32];
    if (dm_ratchet_init(ikm, addr_a, addr_b, rk, ck_lohi, ck_hilo) != 0) {
        crypto_secure_wipe(rk, sizeof(rk));
        crypto_secure_wipe(ck_lohi, sizeof(ck_lohi));
        crypto_secure_wipe(ck_hilo, sizeof(ck_hilo));
        /* Whole-ratchet wipe: clears rk, both chains (valid -> 0), the skip
         * caches, and the retained previous epoch, so no stale or partial key
         * material is reachable and no chain reports itself usable. */
        crypto_secure_wipe(&s->ratchet, sizeof(s->ratchet));
        crypto_secure_wipe(s->session_key, sizeof(s->session_key));
        return -1;
    }
    memcpy(s->ratchet.rk, rk, 32);
    crypto_secure_wipe(rk, sizeof(rk));
    int self_is_lo = (addr_self == addr_a); /* lo sends lohi, receives hilo */
    dm_ratchet_install_chains(&s->ratchet, ck_lohi, ck_hilo, self_is_lo, s->ke_epoch);
    memset(s->ratchet.skip, 0, sizeof(s->ratchet.skip));
    memset(&s->ratchet.prev_recv, 0, sizeof(s->ratchet.prev_recv));
    memset(s->ratchet.prev_skip, 0, sizeof(s->ratchet.prev_skip));
    s->ratchet.new_epoch_msgs = 0;
    /* Retain the legacy static session_key as RK_0 provenance only; the ratchet
     * chains are authoritative for every message now. */
    memcpy(s->session_key, s->ratchet.rk, 32);
    crypto_secure_wipe(ck_lohi, 32);
    crypto_secure_wipe(ck_hilo, 32);
    return 0;
}

/*
 * Fails closed by TEARING THE RATCHET DOWN, not by keeping the old epoch.
 *
 * Both derivations happen into locals before anything on s is touched, so a
 * failure cannot install a garbage root or a chain pair the peer will never
 * agree with. The interesting question is what to do with the surviving old
 * epoch. Keeping it is tempting (the function's contract says a lost rekey
 * leaves both sides on the current epoch and strands nothing) but that contract
 * only holds when the rekey never happened at ALL. By the time this is called
 * the peer has already committed to new_epoch: silently staying on the old one
 * is a ONE-SIDED epoch desync, which is the exact shape of the permanent silent
 * DM loss this repo has already been bitten by, and it would look healthy from
 * the outside (send.valid set, encrypt succeeding, every frame undecryptable at
 * the far end forever).
 *
 * So on failure the whole ratchet is wiped, which drops both chains to
 * valid == 0. dm_session_ratchet_encrypt then refuses to send (surfaced by the
 * mesh caller as an encrypt failure rather than a message that vanishes) and
 * dm_session_ratchet_decrypt returns DM_DECRYPT_FAIL, which the mesh caller
 * already maps to its rate-limited re-handshake desync-heal path. Failure is
 * therefore loud and self-healing rather than quiet and permanent. Returns 0 on
 * success, -1 on a failed derivation.
 */
int dm_session_epoch_bump(dm_session_t* s, const uint8_t new_dh[32], uint32_t addr_self,
                          uint32_t addr_peer, uint16_t new_epoch) {
    uint32_t addr_a = addr_self < addr_peer ? addr_self : addr_peer;
    uint32_t addr_b = addr_self < addr_peer ? addr_peer : addr_self;
    /* Roll the root forward: RK_{e+1} = HKDF(salt=RK_e, ikm=new_dh, info@epoch).
     * Chaining from the CURRENT root plus a fresh DH is the Double Ratchet root
     * KDF and is what makes a pre-bump root compromise recoverable (PCS).
     * Derived into locals: s is not modified until both steps have succeeded. */
    int rc = -1;
    uint8_t rk_next[32];
    uint8_t ck_lohi[32], ck_hilo[32];
    if (dm_ratchet_dh(s->ratchet.rk, new_dh, addr_a, addr_b, new_epoch, rk_next) != 0)
        goto fail;
    if (dm_derive_chain_pair(rk_next, ck_lohi, ck_hilo) != 0)
        goto fail;

    /* Commit. Retain the current receive chain + skip cache as the previous
     * epoch for the grace window (in-flight old-epoch frames still decrypt). */
    s->ratchet.prev_recv = s->ratchet.recv;
    memcpy(s->ratchet.prev_skip, s->ratchet.skip, sizeof(s->ratchet.prev_skip));
    s->ratchet.new_epoch_msgs = 0;
    memcpy(s->ratchet.rk, rk_next, 32);
    int self_is_lo = (addr_self == addr_a);
    dm_ratchet_install_chains(&s->ratchet, ck_lohi, ck_hilo, self_is_lo, new_epoch);
    s->ke_epoch = new_epoch;
    memset(s->ratchet.skip, 0, sizeof(s->ratchet.skip));
    memcpy(s->session_key, s->ratchet.rk, 32); /* provenance mirror, as in init_state */
    rc = 0;
    goto out;
fail:
    /* See the header comment: drop to an unusable ratchet so the peer-visible
     * result is a re-handshake, not silent one-way loss. */
    crypto_secure_wipe(&s->ratchet, sizeof(s->ratchet));
    crypto_secure_wipe(s->session_key, sizeof(s->session_key));
out:
    crypto_secure_wipe(rk_next, 32);
    crypto_secure_wipe(ck_lohi, 32);
    crypto_secure_wipe(ck_hilo, 32);
    return rc;
}

int dm_session_ratchet_encrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr,
                               const uint8_t* pt, size_t pt_len, const uint8_t nonce[12],
                               uint8_t* framed_ct_out, uint8_t* tag_out, size_t* framed_len_out) {
    if (!s->ratchet.send.valid)
        return -1;
    if (pt_len > 255)
        return -1;
    uint8_t mk[32], ck_next[32];
    /* A failed derivation must never reach the AEAD or the chain commit below.
     * Bailing here leaves the send chain and index completely untouched, so this
     * is a clean, retryable no-send: the mesh caller reports an encrypt failure
     * (the message is not silently dropped) and a later attempt on a healthy
     * HKDF picks up exactly where this one left off. */
    if (dm_ratchet_step(s->ratchet.send.ck, s->ratchet.send.index, mk, ck_next) != 0) {
        crypto_secure_wipe(mk, sizeof(mk));
        crypto_secure_wipe(ck_next, sizeof(ck_next));
        return -1;
    }

    /* Cleartext ratchet header: epoch || msg_index (big-endian). */
    uint8_t hdr[DM_RATCHET_HEADER_SIZE];
    hdr[0] = s->ratchet.send.epoch;
    hdr[1] = (uint8_t)(s->ratchet.send.index >> 8);
    hdr[2] = (uint8_t)(s->ratchet.send.index & 0xFF);

    /* AAD = base packet AAD || the 3 cleartext header bytes. The header is
     * authenticated (an attacker cannot flip epoch/index without failing the
     * GCM tag) but NOT encrypted. */
    uint8_t aad[HEADER_SIZE + 4 + DM_RATCHET_HEADER_SIZE];
    if (bramble_build_aead_aad(h, src_addr, aad, HEADER_SIZE + 4) != ESP_OK)
        return -1;
    memcpy(aad + HEADER_SIZE + 4, hdr, DM_RATCHET_HEADER_SIZE);

    /* Encrypt ONLY the payload; the header stays in the clear at the front of
     * the frame, ahead of the ciphertext. */
    int rc = crypto_aes256gcm_encrypt(mk, nonce, pt, pt_len, aad, sizeof(aad),
                                      framed_ct_out + DM_RATCHET_HEADER_SIZE, tag_out);
    if (rc == 0) {
        memcpy(framed_ct_out, hdr, DM_RATCHET_HEADER_SIZE);
        memcpy(s->ratchet.send.ck, ck_next, 32); /* advance; old ck overwritten */
        s->ratchet.send.index++;
        *framed_len_out = DM_RATCHET_HEADER_SIZE + pt_len;
    }
    crypto_secure_wipe(mk, sizeof(mk));
    crypto_secure_wipe(ck_next, sizeof(ck_next));
    return rc;
}

/* Walk one receive chain (chain/skip pair) to the KNOWN cleartext index and do a
 * single decrypt. DM_DECRYPT_OK on success (updates chain + skip); DM_DECRYPT_FAIL
 * if the one derived key does not authenticate (forged / wrong-epoch frame, chain
 * left untouched); DM_DECRYPT_TOO_FAR if index is beyond [next .. next+DM_MAX_SKIP]
 * or is an already-consumed straggler not in the skip cache. */
static int dm_recv_walk(dm_chain_t* chain, dm_skip_entry_t* skip, uint16_t index,
                        const uint8_t* aad, size_t aad_len, const uint8_t nonce[12],
                        const uint8_t* ct, size_t ct_len, const uint8_t* tag, uint8_t* pt_out,
                        size_t* pt_len_out) {
    /* 1) straggler behind the cursor: only the skip cache can hold its key. */
    if (index < chain->index) {
        for (int i = 0; i < DM_MAX_SKIP; i++) {
            if (skip[i].used && skip[i].index == index) {
                if (crypto_aes256gcm_decrypt(skip[i].mk, nonce, ct, ct_len, aad, aad_len, tag,
                                             pt_out) != 0)
                    return DM_DECRYPT_FAIL;
                *pt_len_out = ct_len;
                memset(&skip[i], 0, sizeof(skip[i])); /* single-use: evict */
                return DM_DECRYPT_OK;
            }
        }
        return DM_DECRYPT_TOO_FAR; /* not cached: replay/forgery or already consumed */
    }
    /* 2) too far ahead: refuse without deriving (bounded work / DoS bound). */
    if (index > (uint16_t)(chain->index + DM_MAX_SKIP))
        return DM_DECRYPT_TOO_FAR;
    /* 3) derive keys [next .. index]; only commit chain/skip on a successful tag. */
    uint8_t ck[32];
    memcpy(ck, chain->ck, 32);
    dm_skip_entry_t pending[DM_MAX_SKIP];
    int npending = 0;
    memset(pending, 0, sizeof(pending));
    uint8_t mk[32], ck_next[32];
    int derive_failed = 0;
    for (uint16_t walk = chain->index; walk < index && !derive_failed; walk++) {
        if (dm_ratchet_step(ck, walk, mk, ck_next) != 0) {
            derive_failed = 1;
            break;
        }
        if (npending < DM_MAX_SKIP) {
            pending[npending].index = walk;
            memcpy(pending[npending].mk, mk, 32);
            pending[npending].used = 1;
            npending++;
        }
        memcpy(ck, ck_next, 32);
    }
    /* the target message key */
    if (!derive_failed && dm_ratchet_step(ck, index, mk, ck_next) != 0)
        derive_failed = 1;
    /* A failed derivation commits NOTHING: the chain key, the chain index, and
     * the skip cache are all left exactly as they were, so this frame is simply
     * not decrypted here. Reporting DM_DECRYPT_FAIL routes it into the mesh
     * caller's existing rate-limited desync-heal re-handshake instead of
     * corrupting the receive chain into permanent one-way loss. */
    int rc = derive_failed
                 ? -1
                 : crypto_aes256gcm_decrypt(mk, nonce, ct, ct_len, aad, aad_len, tag, pt_out);
    if (rc == 0) {
        for (int p = 0; p < npending; p++) { /* cache the skipped keys */
            int slot = pending[p].index % DM_MAX_SKIP;
            skip[slot] = pending[p];
            skip[slot].used = 1;
        }
        memcpy(chain->ck, ck_next, 32);
        chain->index = (uint16_t)(index + 1);
        *pt_len_out = ct_len;
    }
    crypto_secure_wipe(ck, 32);
    crypto_secure_wipe(mk, 32);
    crypto_secure_wipe(ck_next, 32);
    crypto_secure_wipe(pending, sizeof(pending));
    return rc == 0 ? DM_DECRYPT_OK : DM_DECRYPT_FAIL;
}

int dm_session_ratchet_decrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr,
                               const uint8_t nonce[12], const uint8_t* framed_ct,
                               size_t framed_ct_len, const uint8_t* tag, uint8_t* pt_out,
                               size_t* pt_len_out) {
    if (!s->ratchet.recv.valid)
        return DM_DECRYPT_FAIL;
    if (framed_ct_len < DM_RATCHET_HEADER_SIZE)
        return DM_DECRYPT_FAIL;
    /* Read the cleartext ratchet header: epoch (framed_ct[0]) || msg_index (BE). */
    uint16_t index = (uint16_t)((framed_ct[1] << 8) | framed_ct[2]);
    const uint8_t* ct = framed_ct + DM_RATCHET_HEADER_SIZE;
    size_t ct_len = framed_ct_len - DM_RATCHET_HEADER_SIZE;
    /* AAD = base packet AAD || the 3 cleartext header bytes (authenticated). */
    uint8_t aad[HEADER_SIZE + 4 + DM_RATCHET_HEADER_SIZE];
    if (bramble_build_aead_aad(h, src_addr, aad, HEADER_SIZE + 4) != ESP_OK)
        return DM_DECRYPT_FAIL;
    memcpy(aad + HEADER_SIZE + 4, framed_ct, DM_RATCHET_HEADER_SIZE);
    int rc = dm_recv_walk(&s->ratchet.recv, s->ratchet.skip, index, aad, sizeof(aad), nonce, ct,
                          ct_len, tag, pt_out, pt_len_out);
    if (rc == DM_DECRYPT_OK) {
        /* A frame authenticated on the CURRENT epoch. Count it toward the grace
         * expiry and, once DM_EPOCH_GRACE_MSGS have been seen, WIPE the previous
         * epoch's retained receive chain + skip cache. This wipe is deferred
         * until AFTER the new chain is established and the grace is spent, which
         * is the ordering that delivers post-compromise secrecy (an attacker who
         * captured the old root can no longer derive any live key). Secure-wipe,
         * not memset: after the clear only prev_recv.valid is ever re-read, so
         * the chain-key and cached-message-key BYTES are dead stores the
         * compiler could otherwise elide, defeating the whole PCS purpose. */
        if (s->ratchet.prev_recv.valid) {
            if (s->ratchet.new_epoch_msgs < 0xFFFF)
                s->ratchet.new_epoch_msgs++;
            if (s->ratchet.new_epoch_msgs >= DM_EPOCH_GRACE_MSGS) {
                crypto_secure_wipe(&s->ratchet.prev_recv, sizeof(s->ratchet.prev_recv));
                crypto_secure_wipe(s->ratchet.prev_skip, sizeof(s->ratchet.prev_skip));
            }
        }
        return DM_DECRYPT_OK;
    }
    /* Current-epoch miss (the derived key did not authenticate, or the index is
     * out of range on the current chain). During the DH-ratchet grace window,
     * retry against the retained previous-epoch chain: an in-flight OLD-epoch
     * frame carries the old epoch byte in its authenticated AAD and only the old
     * chain's keys can decrypt it. GCM authentication makes this retry safe (a
     * current-epoch frame can never falsely authenticate under an old key). */
    if (s->ratchet.prev_recv.valid) {
        size_t prev_pt_len = 0;
        int prc = dm_recv_walk(&s->ratchet.prev_recv, s->ratchet.prev_skip, index, aad, sizeof(aad),
                               nonce, ct, ct_len, tag, pt_out, &prev_pt_len);
        if (prc == DM_DECRYPT_OK) {
            *pt_len_out = prev_pt_len;
            return DM_DECRYPT_OK;
        }
    }
    /* No epoch could decrypt it: return the current-epoch disposition so the mesh
     * caller maps DM_DECRYPT_TOO_FAR / DM_DECRYPT_FAIL to the desync-heal path. */
    return rc;
}

int dm_derive_session_key(const uint8_t my_id_priv[32], const uint8_t my_eph_priv[32],
                          const uint8_t peer_id_pub[32], const uint8_t peer_eph_pub[32],
                          uint32_t addr_a, uint32_t addr_b, uint16_t ke_epoch,
                          uint8_t session_key_out[32]) {
    uint8_t ikm[128];
    if (dm_compute_ikm(my_id_priv, my_eph_priv, peer_id_pub, peer_eph_pub, ikm) != 0)
        return -1;
    int rc = dm_session_key_from_ikm(ikm, addr_a, addr_b, ke_epoch, session_key_out);
    crypto_secure_wipe(ikm, sizeof(ikm));
    return rc;
}

/* Renders 4 HKDF output bytes as 7 human-comparable decimal digits. Shared
 * by dm_derive_sas and dm_derive_identity_sas. */
static void dm_sas_render(const uint8_t okm[4], char sas_out[8]) {
    uint32_t v =
        ((uint32_t)okm[0] << 24) | ((uint32_t)okm[1] << 16) | ((uint32_t)okm[2] << 8) | okm[3];
    snprintf(sas_out, 8, "%07u", (unsigned)(v % 10000000u));
}

int dm_derive_sas(const uint8_t session_ikm[128], char sas_out[8]) {
    const char* salt = "bramble-sas";
    uint8_t okm[4];
    if (crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), session_ikm, 128, NULL, 0, okm,
                           sizeof(okm)) != 0)
        return -1;
    dm_sas_render(okm, sas_out);
    return 0;
}

/*
 * Identity-bound SAS: HKDF(salt="bramble-sas-id", ikm="", info=id_lo||id_hi,
 * L=4), rendered as 7 decimal digits (spec B.1). The two X25519 identity keys
 * are placed in addr_lo/addr_hi order so both peers derive the same value
 * regardless of who calls with which argument first, exactly like dm_build_info
 * canonicalizes addresses. Empty IKM is passed as dm_ratchet_empty_ikm (a
 * non-NULL pointer, length 0): the OpenSSL host HKDF wrapper rejects a NULL key
 * pointer outright, so NULL would fail to derive (see the dm_ratchet chain
 * note above). The SAS is a fingerprint of PUBLIC keys, not a secret, so the
 * plain memcmp ordering below leaks nothing sensitive.
 */
int dm_derive_identity_sas(const uint8_t id_x25519_a[32], const uint8_t id_x25519_b[32],
                           uint32_t addr_a, uint32_t addr_b, char sas_out[8]) {
    const uint8_t* lo = addr_a < addr_b ? id_x25519_a : id_x25519_b;
    const uint8_t* hi = addr_a < addr_b ? id_x25519_b : id_x25519_a;
    uint8_t info[64];
    memcpy(info, lo, 32);
    memcpy(info + 32, hi, 32);
    const char* salt = "bramble-sas-id";
    uint8_t okm[4];
    if (crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), dm_ratchet_empty_ikm, 0, info,
                           sizeof(info), okm, sizeof(okm)) != 0)
        return -1;
    dm_sas_render(okm, sas_out);
    return 0;
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

    int rc = -1;
    uint8_t key[32] = {0};
    uint8_t full_mac[32] = {0};
    const char* salt = "bramble-dm-v2";
    if (crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), ikm, ikm_len, hkdf_info,
                           label_len + sizeof(info), key, sizeof(key)) != 0)
        goto out;

    if (crypto_hmac_sha256(key, sizeof(key), transcript, transcript_len, full_mac) != 0)
        goto out;
    memcpy(tag_out, full_mac, 16);
    rc = 0;
out:
    /* Wipe the HKDF-derived MAC subkey and the full MAC on every exit past
     * their population: only full_mac[0:16] ever leaves this function (as
     * tag_out); the key and the MAC tail are secret scratch. */
    crypto_secure_wipe(key, sizeof(key));
    crypto_secure_wipe(full_mac, sizeof(full_mac));
    return rc;
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
    int rc = -1;
    uint8_t dh2[32] = {0}, dh3[32] = {0};
    uint8_t ikm[64] = {0};
    if (crypto_x25519_dh(my_id->private_key, peer_id_pub_or_null, dh2) != 0)
        goto out;
    if (crypto_x25519_dh(my_eph_priv, peer_id_pub_or_null, dh3) != 0)
        goto out;

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

    rc = dm_derive_ke_tag("bramble-ke-init", ikm, sizeof(ikm), my_id->address, peer_addr, ke_epoch,
                          transcript, sizeof(transcript), out->auth_tag);
out:
    /* Wipe the DH outputs and IKM on every exit past the DH steps (transcript
     * holds only public fields and needs no wipe). */
    crypto_secure_wipe(dh2, sizeof(dh2));
    crypto_secure_wipe(dh3, sizeof(dh3));
    crypto_secure_wipe(ikm, sizeof(ikm));
    return rc;
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

    int rc = -1;
    uint8_t dh2[32] = {0}, dh3[32] = {0};
    uint8_t ikm[64] = {0};
    if (crypto_x25519_dh(my_id->private_key, peer_id_pub, dh2) != 0)
        goto out;
    /* My identity bound to the initiator's ephemeral (msg->ephemeral_pubkey).
     * By X25519 symmetry this equals the initiator's own
     * X25519(their eph_priv, my id_pub) computed in dm_build_init; the
     * verifier needs no ephemeral of its own for this term. */
    if (crypto_x25519_dh(my_id->private_key, msg->ephemeral_pubkey, dh3) != 0)
        goto out;

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
        goto out;

    rc = crypto_ct_memeq(expect_tag, msg->auth_tag, 16) ? 0 : -1;
out:
    /* Wipe the DH outputs and IKM on every exit past the DH steps. */
    crypto_secure_wipe(dh2, sizeof(dh2));
    crypto_secure_wipe(dh3, sizeof(dh3));
    crypto_secure_wipe(ikm, sizeof(ikm));
    return rc;
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

    int rc = -1;
    uint8_t ikm[128];
    if (dm_compute_ikm(my_id->private_key, my_eph_priv, init->long_term_pubkey,
                       init->ephemeral_pubkey, ikm) != 0)
        return -1; /* dm_compute_ikm already wiped ikm on failure */
    if (dm_session_key_from_ikm(ikm, my_id->address, init->src_addr, ke_epoch, session_key_out) !=
        0)
        goto out;

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

    rc = dm_derive_ke_tag("bramble-ke-confirm", ikm, sizeof(ikm), init->src_addr, my_id->address,
                          ke_epoch, transcript, sizeof(transcript), out->auth_tag);
out:
    /* Wipe the IKM on every exit past a successful dm_compute_ikm. */
    crypto_secure_wipe(ikm, sizeof(ikm));
    return rc;
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

    int rc = -1;
    uint8_t local_key[32] = {0};
    uint8_t ikm[128];
    if (dm_compute_ikm(my_id->private_key, my_eph_priv, resp->long_term_pubkey,
                       resp->ephemeral_pubkey, ikm) != 0)
        return -1; /* dm_compute_ikm already wiped ikm on failure */

    /* Compute into a local buffer, not the caller's session_key_out, until
     * the confirm tag verifies: a caller that ignores the return value
     * must never observe an unauthenticated key. No secrecy is at stake
     * either way (this is the verifier's own computation from its own
     * private key), but a discipline of "the output is only valid on
     * success" is worth keeping regardless. */
    if (dm_session_key_from_ikm(ikm, my_id->address, resp->src_addr, ke_epoch, local_key) != 0)
        goto out;

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
        goto out;

    if (!crypto_ct_memeq(expect_tag, resp->auth_tag, 16))
        goto out;

    memcpy(session_key_out, local_key, 32);
    rc = 0;
out:
    /* Wipe the IKM and the local session-key copy on every exit past a
     * successful dm_compute_ikm (local_key after it is copied out on success). */
    crypto_secure_wipe(ikm, sizeof(ikm));
    crypto_secure_wipe(local_key, sizeof(local_key));
    return rc;
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
    /* The whole-struct wipe also zeroes the embedded dm_ratchet_t (root, both
     * chains, skip caches, and the retained previous-epoch state), so forward
     * secrecy on teardown covers every ratchet key, not just session_key.
     * Secure-wipe, not memset: the caller reads nothing back but s->state
     * afterward (it goes to DM_STATE_NONE), so every key byte is a dead store
     * the compiler could elide, which would defeat forward secrecy. */
    crypto_secure_wipe(s, sizeof(*s)); /* state -> DM_STATE_NONE, key + peer_id_pub wiped */
    return true;
}

bool dm_pin_disagrees(const dm_session_t* s, const uint8_t pinned_x25519[32]) {
    /* Only an ESTABLISHED session is authoritative to compare against; a
     * public-key compare (peer_id_pub is not secret), so memcmp is fine. */
    return s->state == DM_STATE_ACTIVE && memcmp(s->peer_id_pub, pinned_x25519, 32) != 0;
}

bool dm_verified_should_clear(const dm_session_t* s, const uint8_t pinned_x25519[32]) {
    return s->verified && dm_pin_disagrees(s, pinned_x25519);
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

    /* Secure-wipe, not memset: unlike the free-slot path above (a DM_STATE_NONE
     * slot was already wiped when it entered NONE and holds no live secret),
     * the victim is a live HANDSHAKING / UNVERIFIED-ACTIVE session whose ratchet
     * keys must be destroyed. Only peer_addr/timestamps are written back after,
     * so the key bytes are dead stores the compiler could otherwise elide. */
    crypto_secure_wipe(&t->s[victim], sizeof(t->s[victim]));
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
