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
#endif
