#ifndef BRAMBLE_IDENTITY_H
#define BRAMBLE_IDENTITY_H

#include "crypto.h"
#include <stddef.h>
#include <stdint.h>

int identity_load(bramble_identity_t* id);
int identity_save(const bramble_identity_t* id);
int identity_generate_and_save(bramble_identity_t* id);
bool identity_check_collision(const bramble_identity_t* my_id, uint32_t beacon_src_addr,
                              uint32_t beacon_pubkey_hash);
/* RPC auth token.
 *
 * identity_ensure_ws_auth_token returns:
 *   1  a fresh token was minted and persisted (first boot)
 *   0  an existing token was loaded, or auth is explicitly disabled
 *      (token_out is empty in the disabled case)
 *  IDENTITY_TOKEN_ERR_STORE    the token store (NVS) could not read or persist
 *  IDENTITY_TOKEN_ERR_ENTROPY  the SEC-L1 entropy gate is shut, so no token
 *                              could be minted; NOTHING was persisted and the
 *                              call is safe to retry once entropy is ready
 * On either error token_out is emptied and callers MUST fail closed: an empty
 * token is not "open access", it is "no credential can match".
 */
#define IDENTITY_TOKEN_ERR_STORE (-1)
#define IDENTITY_TOKEN_ERR_ENTROPY (-2)

int identity_ensure_ws_auth_token(char* token_out, size_t token_out_len);

/* Mint a fresh 32-hex-char token from crypto_random(), the SEC-L1
 * entropy-gated source. Platform-independent (host tests cover the
 * fail-closed path). token_out_len must be >= 33. Returns 0, or
 * IDENTITY_TOKEN_ERR_ENTROPY with token_out emptied when the gate is shut. */
int identity_mint_ws_auth_token(char* token_out, size_t token_out_len);

/* --- Trust-anchor endorsement primitive (trust-anchor campaign, P0) --------
 * A fleet has one Ed25519 ANCHOR keypair. The anchor holder (an offline
 * operator client) signs an endorsement over each node's Ed25519 identity
 * public key, binding "this key is a member of my fleet" to a validity
 * window. The DEVICE never signs endorsements and never holds the anchor
 * PRIVATE key; signing happens only in tests/host/webapp via
 * crypto_ed25519_sign. These helpers are pure (no NVS, no state). */

/* Canonical endorsement signed message: context(18) || node_ed25519_pub(32)
 * || not_after(8, big-endian, ms epoch) = 58 bytes. The context prefix
 * domain-separates this from every other Ed25519 use (the attestation
 * self-signature uses "bramble-ident-v1"; see packet.h). */
#define IDENTITY_ENDORSEMENT_MSG_CONTEXT "bramble-endorse-v1"
#define IDENTITY_ENDORSEMENT_MSG_CONTEXT_LEN 18
#define IDENTITY_ENDORSEMENT_MSG_SIZE (IDENTITY_ENDORSEMENT_MSG_CONTEXT_LEN + 32 + 8) /* 58 */

/* Endorsement certificate as stored/transmitted (wired in later phases):
 * not_after(8, big-endian) || endorsement_sig(64) = 72 bytes. */
#define IDENTITY_ENDORSEMENT_CERT_SIZE (8 + 64) /* 72 */

/* not_after sentinels: UINT64_MAX = PERMANENT (v1 always issues this);
 * 0 = "no cert present". */
#define IDENTITY_ENDORSEMENT_NOT_AFTER_PERMANENT UINT64_MAX
#define IDENTITY_ENDORSEMENT_NOT_AFTER_NONE 0

/* Build the 58-byte canonical endorsement message into buf. Returns
 * IDENTITY_ENDORSEMENT_MSG_SIZE on success, 0 if buf_len is too small. */
size_t identity_endorsement_msg(const uint8_t ed25519_pub[BRAMBLE_ED25519_PUBKEY_SIZE],
                                uint64_t not_after, uint8_t* buf, size_t buf_len);

/* Rebuild the canonical message and verify sig under anchor_pub. Pure. */
bool identity_endorsement_verify(const uint8_t anchor_pub[BRAMBLE_ED25519_PUBKEY_SIZE],
                                 const uint8_t ed25519_pub[BRAMBLE_ED25519_PUBKEY_SIZE],
                                 uint64_t not_after, const uint8_t sig[BRAMBLE_ED25519_SIG_SIZE]);

/* --- Anchor public-key provisioning (trust-anchor campaign, P0) ------------
 * The fleet anchor PUBLIC key, persisted per node (device NVS, host in-memory)
 * and mirrored in module memory. Absent = not anchored = the default; nothing
 * loads or creates one implicitly. Mirrors the network_key provider. */

/* Provision the anchor public key: sets module memory and persists it.
 * Returns 0 on success. In-memory state is authoritative, so a persist
 * failure does not un-anchor (mirrors network_key_set_provisioned). */
int identity_anchor_set(const uint8_t pub[BRAMBLE_ED25519_PUBKEY_SIZE]);

/* Copy the provisioned anchor public key out. Returns 0 iff an anchor is
 * set; on failure returns -1 and leaves out untouched (fail-closed). */
int identity_anchor_get(uint8_t out[BRAMBLE_ED25519_PUBKEY_SIZE]);

/* True iff an anchor public key is provisioned in module memory. */
bool identity_anchor_is_set(void);

/* SHA256(anchor_pub)[0:4] into out. Emits the all-zero sentinel when no
 * anchor is set (mirrors network_key_fingerprint). */
void identity_anchor_fingerprint(uint8_t out[4]);

/* Load the persisted anchor public key into module memory. Returns 0 if one
 * was stored, -1 if none (which must NOT create one). */
int identity_anchor_load(void);

/* Clear the in-memory anchor (does not touch persistence); mirrors
 * network_key_clear. */
void identity_anchor_clear(void);

/* --- Own endorsement certificate (trust-anchor campaign, P1) ----------------
 * The node's OWN cert: the anchor's signature over this node's Ed25519
 * identity key, plus its not_after validity bound. Provisioned via the
 * setEndorsement RPC (after verifying the cert against the node's key and the
 * provisioned anchor), mirrored to the per-platform blob store, and put on
 * the wire in the attestation frame. The device never signs this. Mirrors the
 * anchor functions above (in-memory authoritative, fail-closed, exact-length).
 */

/* Provision the node's own cert: sets module memory and persists it (not_after
 * as an 8-byte big-endian blob, sig as a 64-byte blob). Returns 0 on success.
 * In-memory state is authoritative, so a persist failure does not un-set. */
int identity_endorsement_set(uint64_t not_after, const uint8_t sig[BRAMBLE_ED25519_SIG_SIZE]);

/* Copy the stored cert out. Returns 0 iff a cert is present; on failure
 * returns -1 and leaves the outputs untouched (fail-closed). */
int identity_endorsement_get(uint64_t* not_after, uint8_t sig[BRAMBLE_ED25519_SIG_SIZE]);

/* True iff the node holds its own endorsement cert in module memory. */
bool identity_endorsement_is_set(void);

/* Load the persisted cert into module memory. Returns 0 if one was stored,
 * -1 if none (which must NOT synthesize one). */
int identity_endorsement_load(void);

/* Clear the in-memory cert (does not touch persistence); mirrors
 * identity_anchor_clear. */
void identity_endorsement_clear_mem(void);

#ifndef ESP_PLATFORM
/* Host builds back identity_save/identity_load with an in-memory blob store
 * (unit tests). Reset it to simulate a fresh flash. */
void identity_host_store_reset(void);
#endif

#endif
