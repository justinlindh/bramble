#ifndef BRAMBLE_CRYPTO_H
#define BRAMBLE_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BRAMBLE_KEY_SIZE       32
#define BRAMBLE_ADDR_SIZE      4
#define BRAMBLE_NONCE_SIZE     12
#define BRAMBLE_TAG_SIZE       16
#define BRAMBLE_HMAC_TRUNC     4

typedef struct {
    uint8_t  private_key[BRAMBLE_KEY_SIZE];
    uint8_t  public_key[BRAMBLE_KEY_SIZE];
    uint32_t address;
    uint32_t pubkey_hash;
} bramble_identity_t;

int crypto_generate_identity(bramble_identity_t *id);
uint32_t crypto_derive_address(const uint8_t *public_key);
uint32_t crypto_derive_pubkey_hash(const uint8_t *public_key);
int crypto_x25519_dh(const uint8_t *private_key, const uint8_t *peer_public_key, uint8_t *shared_secret);
int crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len);
int crypto_aes256gcm_encrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *plaintext, size_t pt_len, const uint8_t *aad, size_t aad_len, uint8_t *ciphertext, uint8_t *tag);
int crypto_aes256gcm_decrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *ciphertext, size_t ct_len, const uint8_t *aad, size_t aad_len, const uint8_t *tag, uint8_t *plaintext);
int crypto_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t *mac);
uint32_t crypto_hmac_sha256_trunc4(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len);
int crypto_sha256(const uint8_t *data, size_t data_len, uint8_t *hash);
void crypto_build_nonce(uint32_t src_addr, uint32_t counter, uint8_t *nonce);
int crypto_random(uint8_t *buf, size_t len);

/* Default public channel PSK — well-known, not secret */
#define BRAMBLE_PUBLIC_CHANNEL_PSK "bramble-default"

#endif
