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

/*
 * Verifies message 1 from the responder's side. Always checks address
 * binding (crypto_derive_address(msg->long_term_pubkey) == msg->src_addr):
 * without this an attacker could claim any address with a key of its own
 * choosing. have_peer_id/peer_id_pub select the rekey path: when set, also
 * verifies the HMAC(K_ke_init, transcript_1) tag (constant-time), computing
 * DH3 as X25519(my_id_priv, msg->ephemeral_pubkey), the responder's own
 * identity bound to the initiator's ephemeral just received; by X25519
 * symmetry this equals the initiator's X25519(my_eph_priv, peer_id_pub),
 * so no responder ephemeral is needed to verify it. Returns 0 on success.
 */
int dm_verify_init(const bramble_key_exchange_t* msg, const bramble_identity_t* my_id,
                   int have_peer_id, const uint8_t peer_id_pub[32]);

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
 * Verifies message 2 from the initiator's side: recomputes the session key,
 * verifies the K_confirm tag (constant-time), and address-checks
 * crypto_derive_address(resp->long_term_pubkey) == resp->src_addr. Returns
 * 0 and fills session_key_out on success.
 */
int dm_verify_resp(const bramble_key_exchange_t* resp, const bramble_identity_t* my_id,
                   const uint8_t my_eph_priv[32], const uint8_t my_eph_pub[32],
                   uint16_t ke_epoch, uint8_t session_key_out[32]);

/*
 * DM session table (Task 1.2). State-priority LRU with a bounded
 * DM_STATE_HANDSHAKING count: this is the DoS defense for the session
 * layer (M4 in the crypto design RFC). A spoofed-INIT flood can occupy at
 * most DM_MAX_HANDSHAKING slots regardless of table size, and an
 * ACTIVE session (verified or not; "UNVERIFIED" == state==ACTIVE &&
 * verified==0, still protected) can never be evicted to make room for a
 * new handshake.
 */
#define DM_MAX_SESSIONS 32
#define DM_MAX_HANDSHAKING (DM_MAX_SESSIONS / 4)

#define DM_STATE_NONE 0
#define DM_STATE_HANDSHAKING 1
#define DM_STATE_ACTIVE 2

typedef struct {
    uint32_t peer_addr;
    uint8_t  session_key[32];
    uint8_t  peer_id_pub[32];  /* cached for rekey-path msg1 auth + SAS */
    uint32_t established_ms;   /* also doubles as last-activity for LRU ordering */
    uint32_t msg_count;
    uint16_t ke_epoch;
    uint8_t  state;
    uint8_t  verified;
} dm_session_t;

typedef struct { dm_session_t s[DM_MAX_SESSIONS]; } dm_table_t;

void dm_table_init(dm_table_t* t);

/* Returns the used slot for peer_addr, or NULL if none. */
dm_session_t* dm_lookup(dm_table_t* t, uint32_t peer_addr);

/*
 * Returns a slot for peer_addr: an existing slot for that peer if one
 * already exists (no cap check; not a new handshake), else a free
 * (DM_STATE_NONE) slot, else the oldest slot (by established_ms) that is
 * NEITHER DM_STATE_ACTIVE NOR UNVERIFIED (state==ACTIVE && verified==0) is
 * LRU-evicted (in practice this means only a DM_STATE_HANDSHAKING slot is
 * ever evictable, since ACTIVE covers UNVERIFIED as a subset). Returns NULL
 * if a brand-new slot (free or evicted) would push the table's
 * DM_STATE_HANDSHAKING count past DM_MAX_HANDSHAKING, or if the table is
 * full with no evictable slot. The caller is responsible for transitioning
 * a freshly-returned slot's state (typically to DM_STATE_HANDSHAKING)
 * immediately; dm_alloc itself only initializes peer_addr/established_ms.
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
                       const uint8_t* pt, size_t pt_len, const uint8_t nonce[12],
                       uint8_t* ct_out, uint8_t* tag_out);

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
