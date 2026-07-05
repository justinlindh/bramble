#ifndef BRAMBLE_CRYPTO_H
#define BRAMBLE_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BRAMBLE_KEY_SIZE 32
#define BRAMBLE_ADDR_SIZE 4
#define BRAMBLE_NONCE_SIZE 12
#define BRAMBLE_TAG_SIZE 16
#define BRAMBLE_HMAC_TRUNC 4

/* Ed25519 signature primitive (RFC 8032). The 64-byte secret key uses the
 * libsodium layout: seed (32) || public key (32). Both backends (device
 * libsodium, host OpenSSL) produce and consume this same layout, pinned by
 * the shared RFC 8032 vectors in test/test_ed25519.c. */
#define BRAMBLE_ED25519_PUBKEY_SIZE 32
#define BRAMBLE_ED25519_SECKEY_SIZE 64
#define BRAMBLE_ED25519_SIG_SIZE 64

typedef struct {
    uint8_t private_key[BRAMBLE_KEY_SIZE]; /* X25519 (DM sessions / DH only) */
    uint8_t public_key[BRAMBLE_KEY_SIZE];  /* X25519 (DM sessions / DH only) */
    /* Ed25519 signing identity (Phase 1). Since the Phase 4 rebind this is
     * THE identity key: the node address derives from it, and identity
     * attestations are signed with it, binding address to keyholder. */
    uint8_t ed25519_public_key[BRAMBLE_ED25519_PUBKEY_SIZE];
    uint8_t ed25519_private_key[BRAMBLE_ED25519_SECKEY_SIZE];
    uint32_t address;     /* crypto_derive_address(ed25519_public_key), SHA256[0:4] */
    uint32_t pubkey_hash; /* crypto_derive_pubkey_hash(ed25519_public_key), SHA256[4:8] */
} bramble_identity_t;

int crypto_generate_identity(bramble_identity_t* id);
uint32_t crypto_derive_address(const uint8_t* public_key);
uint32_t crypto_derive_pubkey_hash(const uint8_t* public_key);
int crypto_x25519_dh(const uint8_t* private_key, const uint8_t* peer_public_key,
                     uint8_t* shared_secret);

/* Returns 0 if the X25519 shared secret is contributory (non-zero), -1 if it
 * is the all-zero low-order result (RFC 7748 contributory check). Constant-
 * time accumulate. Shared by host and device builds; the device (mbedtls)
 * path is the one that needs it, since mbedtls does not reject low-order
 * peer points on its own. */
static inline int crypto_x25519_check_shared(const uint8_t ss[32]) {
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++)
        acc |= ss[i];
    return acc == 0 ? -1 : 0;
}
int crypto_hkdf_sha256(const uint8_t* salt, size_t salt_len, const uint8_t* ikm, size_t ikm_len,
                       const uint8_t* info, size_t info_len, uint8_t* okm, size_t okm_len);
int crypto_aes256gcm_encrypt(const uint8_t* key, const uint8_t* nonce, const uint8_t* plaintext,
                             size_t pt_len, const uint8_t* aad, size_t aad_len, uint8_t* ciphertext,
                             uint8_t* tag);
int crypto_aes256gcm_decrypt(const uint8_t* key, const uint8_t* nonce, const uint8_t* ciphertext,
                             size_t ct_len, const uint8_t* aad, size_t aad_len, const uint8_t* tag,
                             uint8_t* plaintext);
int crypto_hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len,
                       uint8_t* mac);
uint32_t crypto_hmac_sha256_trunc4(const uint8_t* key, size_t key_len, const uint8_t* data,
                                   size_t data_len);
int crypto_sha256(const uint8_t* data, size_t data_len, uint8_t* hash);
int crypto_random(uint8_t* buf, size_t len);

/* Ed25519 sign/verify/keypair. Keygen draws its 32-byte seed from
 * crypto_random(), so on device it sits behind the SEC-L1 fail-closed
 * entropy gate (crypto_entropy_fill); returns nonzero and writes no key
 * material when the gate is shut. Verify rejects non-canonical (S >= L)
 * signatures on both backends. */
int crypto_ed25519_keypair(uint8_t public_key[BRAMBLE_ED25519_PUBKEY_SIZE],
                           uint8_t private_key[BRAMBLE_ED25519_SECKEY_SIZE]);
int crypto_ed25519_sign(const uint8_t private_key[BRAMBLE_ED25519_SECKEY_SIZE], const uint8_t* msg,
                        size_t msg_len, uint8_t sig[BRAMBLE_ED25519_SIG_SIZE]);
bool crypto_ed25519_verify(const uint8_t public_key[BRAMBLE_ED25519_PUBKEY_SIZE],
                           const uint8_t* msg, size_t msg_len,
                           const uint8_t sig[BRAMBLE_ED25519_SIG_SIZE]);

/* Default public channel PSK — well-known, not secret */
#define BRAMBLE_PUBLIC_CHANNEL_PSK "bramble-default"

#endif
