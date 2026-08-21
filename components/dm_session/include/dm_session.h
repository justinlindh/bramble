#ifndef BRAMBLE_DM_SESSION_H
#define BRAMBLE_DM_SESSION_H
#include <stdint.h>
#include "crypto.h"
#include "packet.h" /* bramble_key_exchange_t */

int dm_derive_session_key(const uint8_t my_id_priv[32], const uint8_t my_eph_priv[32],
                          const uint8_t peer_id_pub[32], const uint8_t peer_eph_pub[32],
                          uint32_t addr_a, uint32_t addr_b, uint16_t ke_epoch,
                          uint8_t session_key_out[32]);
int dm_derive_sas(const uint8_t session_ikm[128], char sas_out[8]);

/*
 * Identity-bound SAS (safety number). Renders a 7-digit decimal fingerprint of
 * the two peers' pinned X25519 identity keys, ordered by their owners'
 * addresses (addr_lo/addr_hi), so both peers compute the same string regardless
 * of argument order. Unlike dm_derive_sas (which commits to the session IKM and
 * so changes on every re-handshake), this depends ONLY on the two identity
 * keys, so it is stable across ratchet steps, epoch bumps, desync-heal, and
 * reboot. It is public-key material, a fingerprint like a Signal safety number,
 * not a secret. Returns 0 on success. sas_out is a 7-digit string plus NUL.
 */
int dm_derive_identity_sas(const uint8_t id_x25519_a[32], const uint8_t id_x25519_b[32],
                           uint32_t addr_a, uint32_t addr_b, char sas_out[8]);

/*
 * Symmetric double-ratchet key schedule (per-message forward secrecy).
 *
 * dm_ratchet_init derives the epoch-0 root RK_0 and the two directional chain
 * keys from the 128-byte handshake IKM. RK_0 is bit-identical to
 * dm_session_key_from_ikm(ikm, addr_a, addr_b, 0): epoch 0 is migration-
 * continuous with the pre-ratchet session key (design A.6). The two chains are
 * domain-separated by the address ordering: CK_0[lo->hi] and CK_0[hi->lo].
 *
 * dm_ratchet_step derives the message key mk_n for index n on a chain and the
 * next chain key CK_{n+1}. mk_n is used once for one AES-256-GCM message, then
 * wiped; CK_n is wiped once CK_{n+1} exists. Returns 0 on success and -1 if
 * either HKDF fails, in which case BOTH outputs are wiped: neither is usable
 * key material and a caller that ignores the status would otherwise encrypt
 * under, and commit, uninitialized stack bytes.
 *
 * dm_ratchet_dh advances the root on an epoch bump: RK_{e+1} folds a fresh
 * X25519 output dh into RK_e (post-compromise recovery at epoch granularity).
 */
int dm_ratchet_init(const uint8_t ikm[128], uint32_t addr_a, uint32_t addr_b, uint8_t rk_out[32],
                    uint8_t ck_lohi_out[32], uint8_t ck_hilo_out[32]);
int dm_ratchet_step(const uint8_t ck_in[32], uint16_t index_n, uint8_t mk_out[32],
                    uint8_t ck_next_out[32]);
int dm_ratchet_dh(const uint8_t rk_e[32], const uint8_t dh[32], uint32_t addr_a, uint32_t addr_b,
                  uint16_t new_epoch, uint8_t rk_next_out[32]);

/*
 * Computes the 128-byte quad-DH handshake IKM (the same schedule dm_build_resp/
 * dm_verify_resp use internally). Exposed non-static so the mesh DH-ratchet
 * epoch path (Task 4) and the ratchet host tests can seed a session's ratchet
 * state directly from a handshake IKM. Returns 0 on success.
 */
int dm_compute_ikm(const uint8_t my_id_priv[32], const uint8_t my_eph_priv[32],
                   const uint8_t peer_id_pub[32], const uint8_t peer_eph_pub[32],
                   uint8_t ikm_out[128]);

/*
 * DM handshake (Task 1.3): INIT/RESP message build and authenticated verify.
 * RFC section 1's message flow, PART 1's B1 construction.
 */
#define KE_TYPE_INIT 1
#define KE_TYPE_RESP 2

/*
 * Builds message 1 (INIT). peer_id_pub_or_null == NULL means first contact
 * (the initiator has no cached identity key for peer_addr): auth_tag is
 * zeroed, since no peer-keyed DH is computable yet (RFC section 1: "message
 * 1 on first contact proves nothing; the initiator is authenticated later,
 * message 3"). Non-NULL means rekey/known-peer: auth_tag is
 * HMAC(K_ke_init, transcript_1)[0:16]. my_eph_priv is required even though
 * it is not embedded on the wire: the rekey tag's DH3 term is
 * X25519(my_eph_priv, peer_id_pub), the initiator's ephemeral bound to the
 * peer's identity, computable at INIT time because it needs no responder
 * ephemeral.
 */
int dm_build_init(const bramble_identity_t* my_id, const uint8_t my_eph_pub[32],
                  const uint8_t my_eph_priv[32], uint32_t peer_addr, uint16_t ke_epoch,
                  const uint8_t* peer_id_pub_or_null, bramble_key_exchange_t* out);

/* Distinct failure code for the Phase 4 pin-continuity check below, so the
 * caller can log a key-change red flag differently from a generic verify
 * failure. */
#define DM_VERIFY_ERR_PIN_MISMATCH (-2)

/*
 * Verifies message 1 from the responder's side.
 *
 * Address<->key binding (Phase 4 rebind): the node address derives from
 * the Ed25519 identity key, NOT from long_term_pubkey (X25519), so the
 * pre-rebind check crypto_derive_address(long_term_pubkey) == src_addr is
 * gone; an X25519 key no longer proves an address by hashing. In its place
 * pinned_peer_x25519_or_null carries the identity store's pinned X25519
 * key for msg->src_addr when one exists (mesh_task snapshots it from the
 * attestation-verified pin store): non-NULL means REQUIRE
 * msg->long_term_pubkey to equal it byte for byte, else fail with
 * DM_VERIFY_ERR_PIN_MISMATCH (a pinned peer whose DM key changed is a red
 * flag, never silently accepted). NULL means no pin exists: proceed
 * TOFU-grade (stated residual: the first-contact window is unauthenticated
 * until the peer's attestation is heard and pinned).
 *
 * have_peer_id/peer_id_pub select the rekey path: when set, also verifies
 * the HMAC(K_ke_init, transcript_1) tag (constant-time), computing DH3 as
 * X25519(my_id_priv, msg->ephemeral_pubkey), the responder's own identity
 * bound to the initiator's ephemeral just received; by X25519 symmetry
 * this equals the initiator's X25519(my_eph_priv, peer_id_pub), so no
 * responder ephemeral is needed to verify it. Returns 0 on success.
 */
int dm_verify_init(const bramble_key_exchange_t* msg, const bramble_identity_t* my_id,
                   int have_peer_id, const uint8_t peer_id_pub[32],
                   const uint8_t* pinned_peer_x25519_or_null);

/*
 * Builds message 2 (RESP): computes the full quad-DH session key (the same
 * schedule as dm_derive_session_key) and sets
 * auth_tag = HMAC(K_confirm, transcript_2)[0:16], a real key-confirmation
 * since the responder has everything (both ephemerals, both identities) by
 * this point.
 */
int dm_build_resp(const bramble_identity_t* my_id, const uint8_t my_eph_pub[32],
                  const uint8_t my_eph_priv[32], const bramble_key_exchange_t* init,
                  uint16_t ke_epoch, bramble_key_exchange_t* out, uint8_t session_key_out[32]);

/*
 * Verifies message 2 from the initiator's side: recomputes the session key
 * and verifies the K_confirm tag (constant-time). src_addr integrity comes
 * from transcript_2 (the tag binds both addresses), and the Phase 4
 * pin-continuity check applies exactly as in dm_verify_init:
 * pinned_peer_x25519_or_null non-NULL requires resp->long_term_pubkey to
 * match the pinned key (DM_VERIFY_ERR_PIN_MISMATCH otherwise), NULL is the
 * no-pin TOFU-grade residual. Returns 0 and fills session_key_out on
 * success.
 */
int dm_verify_resp(const bramble_key_exchange_t* resp, const bramble_identity_t* my_id,
                   const uint8_t my_eph_priv[32], const uint8_t my_eph_pub[32], uint16_t ke_epoch,
                   const uint8_t* pinned_peer_x25519_or_null, uint8_t session_key_out[32]);

/*
 * DM session table (Task 1.2). State-priority LRU with a bounded
 * DM_STATE_HANDSHAKING count: this is the DoS defense for the session
 * layer (M4 in the crypto design RFC). A spoofed-INIT flood can occupy at
 * most DM_MAX_HANDSHAKING slots regardless of table size.
 *
 * Fix 1 (red-team panel, post-Task-3.6): only a VERIFIED ACTIVE session
 * (state==ACTIVE && verified==1) is fully protected from eviction. An
 * UNVERIFIED ACTIVE session (state==ACTIVE && verified==0) IS evictable
 * under allocation pressure, LRU-ordered by last_active_ms, same pool as
 * HANDSHAKING slots. This closes a session-table exhaustion DoS: a
 * first-contact INIT (main/mesh_task.c's process_ke_init) needs no secret
 * and goes straight to ACTIVE/verified=0 without ever touching
 * DM_STATE_HANDSHAKING or its cap, so DM_MAX_SESSIONS forged first-contact
 * INITs from freshly-generated identities used to fill the table with
 * permanently-unevictable slots (every ACTIVE session was protected
 * regardless of verified), killing all future DM establishment until
 * reboot. Bumping last_active_ms on every real send/receive (the caller's
 * job, under s_dm_mutex) means a genuinely-active UNVERIFIED session still
 * outlives an idle attacker flood: eviction pressure reclaims stale forged
 * slots first.
 */
#define DM_MAX_SESSIONS 32
#define DM_MAX_HANDSHAKING (DM_MAX_SESSIONS / 4)

#define DM_STATE_NONE 0
#define DM_STATE_HANDSHAKING 1
#define DM_STATE_ACTIVE 2

/*
 * Symmetric-ratchet runtime state embedded in every dm_session_t slot. A send
 * chain and a receive chain (directionally domain-separated by address order),
 * plus a bounded skip cache for out-of-order / lost LoRa frames. DM_MAX_SKIP is
 * both the forward-derive bound and the skip-cache size: it is a loss-tolerance
 * vs RAM tuning constant, NOT a security boundary (an index beyond
 * next+DM_MAX_SKIP is refused and degrades to the desync-heal re-handshake).
 * prev_recv/prev_skip retain the previous epoch's receive chain and skip cache
 * across the bounded DH-ratchet grace window, so in-flight old-epoch frames
 * still decrypt until the previous epoch is wiped.
 */
#define DM_MAX_SKIP 16

typedef struct {
    uint8_t ck[32]; /* current chain key */
    uint16_t index; /* next index to send / next expected to receive */
    uint8_t epoch;  /* low byte of ke_epoch this chain belongs to */
    uint8_t valid;  /* 0 until dm_session_ratchet_init_state establishes it */
} dm_chain_t;

typedef struct {
    uint16_t index;
    uint8_t mk[32];
    uint8_t used;
} dm_skip_entry_t;

typedef struct {
    uint8_t rk[32];
    dm_chain_t send;
    dm_chain_t recv;
    dm_skip_entry_t skip[DM_MAX_SKIP];
    /* Previous-epoch receive retention during the DH-ratchet grace (Task 4). */
    dm_chain_t prev_recv;
    dm_skip_entry_t prev_skip[DM_MAX_SKIP];
    uint16_t new_epoch_msgs; /* messages seen on the new epoch; grace expiry */
} dm_ratchet_t;

typedef struct {
    uint32_t peer_addr;
    uint8_t session_key[32];
    uint8_t peer_id_pub[32]; /* cached for rekey-path msg1 auth + SAS */
    uint32_t established_ms; /* when this slot was (re)established, informational only */
    /* Fix 1 (red-team panel): set at allocation, and bumped by the caller
     * (main/mesh_task.c, under s_dm_mutex) on every successful send or
     * receive through this session. Drives dm_alloc's eviction ordering,
     * separately from established_ms, so a genuinely-active session is
     * never the LRU victim just because it was established first. */
    uint32_t last_active_ms;
    uint32_t msg_count;
    uint16_t ke_epoch;
    uint8_t state;
    uint8_t verified;
    dm_ratchet_t ratchet;
} dm_session_t;

typedef struct {
    dm_session_t s[DM_MAX_SESSIONS];
} dm_table_t;

void dm_table_init(dm_table_t* t);

/* Returns the used slot for peer_addr, or NULL if none. */
dm_session_t* dm_lookup(dm_table_t* t, uint32_t peer_addr);

/*
 * M2 TOFU-session teardown (P3b): drop the slot for peer_addr (same match
 * dm_lookup uses), zeroing it back to DM_STATE_NONE. Returns true if a slot
 * was found and torn down, false if none existed. Operates on the passed
 * table only (no globals), so it is host-testable; the mesh_task caller
 * holds s_dm_mutex across the lookup+decision+teardown. This is fail-safe
 * defense-in-depth: it only ever DROPS a session (recovered by re-handshake),
 * never establishes or mutates one in place.
 */
bool dm_session_teardown(dm_table_t* t, uint32_t peer_addr);

/*
 * The pure M2 decision, extracted so it is host-testable (the mesh_task hook
 * that calls it is board-build-only): true iff s is an ESTABLISHED session
 * (DM_STATE_ACTIVE) whose cached peer X25519 key (s->peer_id_pub, the
 * long_term_pubkey accepted at first contact) DISAGREES with the pinned,
 * attestation-authenticated binding pinned_x25519. A matching key is the
 * healthy pinned session (false); a non-ACTIVE slot is never a teardown
 * target (false) since dm_alloc's LRU already reclaims handshaking slots and
 * a mid-handshake worker holds its own assumptions about the slot.
 */
bool dm_pin_disagrees(const dm_session_t* s, const uint8_t pinned_x25519[32]);

/*
 * Task 7 decision: whether a genuine pin key change must clear the
 * session's verified bit, true iff s->verified AND dm_pin_disagrees. A
 * session that is not verified has nothing to clear; a verified session
 * whose pin still matches keeps its verified bit (ratchet steps, epoch
 * bumps, desync-heal, and reboot all keep the same pinned identity key,
 * so the identity SAS is unchanged and re-verification is not needed).
 * Only an actual key change (CONFLICT / rebind) is the red flag.
 */
bool dm_verified_should_clear(const dm_session_t* s, const uint8_t pinned_x25519[32]);

/*
 * Returns a slot for peer_addr: an existing slot for that peer if one
 * already exists (no cap check; not a new handshake), else a free
 * (DM_STATE_NONE) slot, else the slot with the smallest last_active_ms that
 * is NOT VERIFIED ACTIVE (state==ACTIVE && verified==1) is LRU-evicted:
 * HANDSHAKING and UNVERIFIED ACTIVE (state==ACTIVE && verified==0) slots
 * are both eligible victims (Fix 1, see the table's doc comment above).
 * Returns NULL if a brand-new slot (free or evicted) would push the
 * table's DM_STATE_HANDSHAKING count past DM_MAX_HANDSHAKING, or if the
 * table is full with no evictable slot (every slot VERIFIED ACTIVE). The
 * caller is responsible for transitioning a freshly-returned slot's state
 * (typically to DM_STATE_HANDSHAKING) immediately, and for bumping
 * last_active_ms on every subsequent send/receive; dm_alloc itself only
 * initializes peer_addr/established_ms/last_active_ms.
 */
dm_session_t* dm_alloc(dm_table_t* t, uint32_t peer_addr, uint32_t now_ms);

/*
 * Task 1.4: thin AES-256-GCM wrappers over an established session key,
 * AAD built via bramble_build_aead_aad (the same SEC-M2 src_addr-bound AAD
 * DATA envelopes use under the channel key). src_addr is the WIRE src_addr
 * field carried alongside the DATA envelope, not a session_t member (a
 * session only stores the peer's address, not "my own"), so encrypt takes
 * it explicitly, matching decrypt's own parameter (which needs it to
 * rebuild the identical AAD the sender authenticated).
 */
int dm_session_encrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr,
                       const uint8_t* pt, size_t pt_len, const uint8_t nonce[12], uint8_t* ct_out,
                       uint8_t* tag_out);

/*
 * Decrypts under s->session_key. src_addr must be the value read off the
 * wire (the DATA envelope's src_addr field): it is authenticated as part
 * of the AAD, so a tampered src_addr fails the GCM tag rather than being
 * silently accepted.
 */
int dm_session_decrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr,
                       const uint8_t nonce[12], const uint8_t* ct, size_t ct_len,
                       const uint8_t* tag, uint8_t* pt_out);

/*
 * Establishes the send/receive ratchet chains on a session from a handshake
 * IKM. addr_self/addr_peer pick which directional chain is the send chain: the
 * lo party (min address) sends on the lohi chain and receives on hilo, the hi
 * party vice versa, so both sides agree. Sets both chain indices to 0, stamps
 * each chain's epoch from s->ke_epoch, and mirrors RK_0 into s->session_key for
 * provenance. Call after a successful handshake, before the first ratchet op.
 *
 * Returns 0 on success, -1 if any derivation fails. On -1 the session's whole
 * ratchet (root, both chains, skip caches, retained previous epoch) and its
 * session_key mirror are wiped, so both chains report valid == 0 and every
 * ratchet operation refuses. The caller MUST NOT treat such a session as
 * established: it holds no keys at all, and the peer must re-handshake. Nothing
 * is installed unless every derivation succeeded, so this can never leave a
 * session that looks valid while holding zero or garbage chain keys.
 */
int dm_session_ratchet_init_state(dm_session_t* s, const uint8_t ikm[128], uint32_t addr_self,
                                  uint32_t addr_peer);

/*
 * Sender-side ratchet encrypt (per-message forward secrecy). Derives the next
 * send message key via dm_ratchet_step, advances the send chain, writes the
 * 3-byte cleartext ratchet header (epoch || msg_index, big-endian) at the front
 * of framed_ct_out, feeds those 3 bytes into the AEAD AAD (so they are
 * authenticated but not encrypted), and encrypts ONLY the payload into the
 * bytes after the header. On success framed_len_out = DM_RATCHET_HEADER_SIZE +
 * pt_len (cleartext header || ciphertext) and tag_out holds the GCM tag. nonce
 * stays the node-global monotonic counter; the message key changes per message,
 * so no (key, nonce) pair ever repeats. Returns 0 on success, -1 otherwise.
 */
int dm_session_ratchet_encrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr,
                               const uint8_t* pt, size_t pt_len, const uint8_t nonce[12],
                               uint8_t* framed_ct_out, uint8_t* tag_out, size_t* framed_len_out);

/*
 * Receiver-side ratchet decrypt with a bounded skip / out-of-order window.
 * framed_ct is the on-wire frame (3-byte cleartext ratchet header || ciphertext);
 * the receiver reads epoch||index from that header FIRST, so key selection is
 * known up front and there is NO trial-decryption loop. It derives EXACTLY the
 * one message key the index names (caching any skipped keys it passes into the
 * per-direction skip cache), does a SINGLE GCM decrypt with the header in the
 * AAD, and writes only the payload to pt_out.
 *
 * Return codes map to the mesh caller's dispositions:
 *   DM_DECRYPT_OK        payload decrypted; chain / skip cache updated.
 *   DM_DECRYPT_FAIL      the one derived key did not authenticate (forged or
 *                        wrong-epoch frame); chain / skip state left untouched.
 *   DM_DECRYPT_TOO_FAR   index > next+DM_MAX_SKIP (refused WITHOUT deriving: the
 *                        DoS bound), or an already-consumed straggler not in the
 *                        skip cache. The caller degrades this into
 *                        maybe_trigger_dm_rehandshake (desync heal).
 *   DM_DECRYPT_REPLAY    reserved for the caller's per-sender nonce-window hit
 *                        (the authoritative replay defense, consulted BEFORE this
 *                        layer); this function never returns it. The ratchet
 *                        index is an ordering aid, not a second replay oracle.
 */
#define DM_DECRYPT_OK 0
#define DM_DECRYPT_FAIL (-1)
#define DM_DECRYPT_REPLAY (-2)
#define DM_DECRYPT_TOO_FAR (-3)
int dm_session_ratchet_decrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr,
                               const uint8_t nonce[12], const uint8_t* framed_ct,
                               size_t framed_ct_len, const uint8_t* tag, uint8_t* pt_out,
                               size_t* pt_len_out);

/*
 * DH-ratchet epoch bump (Task 4, post-compromise recovery at epoch
 * granularity). Rolls the root forward by folding a fresh X25519 output new_dh
 * into the CURRENT root (dm_ratchet_dh), retains the current receive chain +
 * skip cache as prev_recv/prev_skip so in-flight OLD-epoch frames still decrypt
 * during a bounded grace, and resets both directional send/recv chains to index
 * 0 on new_epoch. Both peers call this with the same new_dh (the fresh
 * ephemeral-ephemeral DH of the rekey handshake, ikm[0:32]) and new_epoch, so
 * they converge on the same root. The old root/chains are overwritten here; the
 * previous epoch's retained recv chain is wiped once DM_EPOCH_GRACE_MSGS
 * new-epoch messages have been seen (the wipe is what delivers PCS). A lost or
 * failed rekey leaves both sides on the current epoch (chains untouched), so no
 * message is ever stranded.
 *
 * Returns 0 on success, -1 if a derivation fails. Nothing is installed unless
 * both the root roll and the chain-pair derivation succeeded. On -1 the whole
 * ratchet is WIPED rather than left on the old epoch: the peer has already
 * committed to new_epoch by this point, so retaining the old chains would be a
 * one-sided epoch desync that looks perfectly healthy locally while every frame
 * is undecryptable at the far end forever. A wiped ratchet instead makes sends
 * fail visibly and makes receives return DM_DECRYPT_FAIL, which the mesh caller
 * already routes into its rate-limited re-handshake desync heal.
 */
#define DM_EPOCH_GRACE_MSGS DM_MAX_SKIP
int dm_session_epoch_bump(dm_session_t* s, const uint8_t new_dh[32], uint32_t addr_self,
                          uint32_t addr_peer, uint16_t new_epoch);
#endif
