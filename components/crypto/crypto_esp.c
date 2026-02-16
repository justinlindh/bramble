// ESP32 crypto implementation using mbedtls (hardware-accelerated on ESP32-S3)
#ifdef ESP_PLATFORM

#include "crypto.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "esp_random.h"
#include <string.h>

int crypto_sha256(const uint8_t *data, size_t data_len, uint8_t *hash) {
    mbedtls_sha256(data, data_len, hash, 0);
    return 0;
}

uint32_t crypto_derive_address(const uint8_t *public_key) {
    uint8_t hash[32];
    crypto_sha256(public_key, 32, hash);
    return ((uint32_t)hash[0] << 24) | ((uint32_t)hash[1] << 16) |
           ((uint32_t)hash[2] << 8)  | (uint32_t)hash[3];
}

uint32_t crypto_derive_pubkey_hash(const uint8_t *public_key) {
    return crypto_derive_address(public_key);
}

int crypto_aes256gcm_encrypt(const uint8_t *key, const uint8_t *nonce,
                             const uint8_t *plaintext, size_t pt_len,
                             const uint8_t *aad, size_t aad_len,
                             uint8_t *ciphertext, uint8_t *tag) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    int ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
        pt_len, nonce, 12, aad, aad_len, plaintext, ciphertext, 16, tag);
    mbedtls_gcm_free(&ctx);
    return (ret == 0) ? 0 : -1;
}

int crypto_aes256gcm_decrypt(const uint8_t *key, const uint8_t *nonce,
                             const uint8_t *ciphertext, size_t ct_len,
                             const uint8_t *aad, size_t aad_len,
                             const uint8_t *tag, uint8_t *plaintext) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    int ret = mbedtls_gcm_auth_decrypt(&ctx, ct_len, nonce, 12,
        aad, aad_len, tag, 16, ciphertext, plaintext);
    mbedtls_gcm_free(&ctx);
    return (ret == 0) ? 0 : -1;
}

int crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *data, size_t data_len,
                       uint8_t *mac) {
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return mbedtls_md_hmac(md_info, key, key_len, data, data_len, mac);
}

uint32_t crypto_hmac_sha256_trunc4(const uint8_t *key, size_t key_len,
                                   const uint8_t *data, size_t data_len) {
    uint8_t mac[32];
    crypto_hmac_sha256(key, key_len, data, data_len, mac);
    return ((uint32_t)mac[0] << 24) | ((uint32_t)mac[1] << 16) |
           ((uint32_t)mac[2] << 8)  | (uint32_t)mac[3];
}

int crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                       const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *info, size_t info_len,
                       uint8_t *okm, size_t okm_len) {
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return mbedtls_hkdf(md_info, salt, salt_len, ikm, ikm_len, info, info_len, okm, okm_len);
}

int crypto_random(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i += 4) {
        uint32_t r = esp_random();
        size_t remaining = len - i;
        size_t to_copy = remaining < 4 ? remaining : 4;
        memcpy(buf + i, &r, to_copy);
    }
    return 0;
}

void crypto_build_nonce(uint32_t src_addr, uint32_t counter, uint8_t *nonce) {
    nonce[0] = (src_addr >> 24) & 0xFF;
    nonce[1] = (src_addr >> 16) & 0xFF;
    nonce[2] = (src_addr >> 8)  & 0xFF;
    nonce[3] = src_addr & 0xFF;
    nonce[4] = (counter >> 24) & 0xFF;
    nonce[5] = (counter >> 16) & 0xFF;
    nonce[6] = (counter >> 8)  & 0xFF;
    nonce[7] = counter & 0xFF;
    crypto_random(&nonce[8], 4);
}

int crypto_x25519_dh(const uint8_t *private_key, const uint8_t *peer_public_key,
                     uint8_t *shared_secret) {
    int ret = -1;
    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point Qp, R;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&Qp);
    mbedtls_ecp_point_init(&R);

    mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    mbedtls_mpi_read_binary_le(&d, private_key, 32);
    mbedtls_mpi_read_binary_le(&Qp.MBEDTLS_PRIVATE(X), peer_public_key, 32);
    mbedtls_mpi_lset(&Qp.MBEDTLS_PRIVATE(Z), 1);

    if (mbedtls_ecp_mul(&grp, &R, &d, &Qp, NULL, NULL) == 0) {
        uint8_t buf[32];
        if (mbedtls_mpi_write_binary_le(&R.MBEDTLS_PRIVATE(X), buf, 32) == 0) {
            memcpy(shared_secret, buf, 32);
            ret = 0;
        }
    }

    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&Qp);
    mbedtls_ecp_point_free(&R);
    return ret;
}

int crypto_generate_identity(bramble_identity_t *id) {
    crypto_random(id->private_key, 32);
    // Clamp per X25519 spec
    id->private_key[0]  &= 248;
    id->private_key[31] &= 127;
    id->private_key[31] |= 64;

    // Compute public key = private_key * basepoint
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    mbedtls_mpi_read_binary_le(&d, id->private_key, 32);

    int ret = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, NULL, NULL);
    if (ret == 0) {
        mbedtls_mpi_write_binary_le(&Q.MBEDTLS_PRIVATE(X), id->public_key, 32);
    }

    id->address = crypto_derive_address(id->public_key);
    id->pubkey_hash = crypto_derive_pubkey_hash(id->public_key);

    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    return (ret == 0) ? 0 : -1;
}

#endif // ESP_PLATFORM
