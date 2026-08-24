#ifndef ESP_PLATFORM

#include "crypto.h"
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/kdf.h>
#include <openssl/crypto.h>

int crypto_random(uint8_t* buf, size_t len) { return RAND_bytes(buf, (int)len) == 1 ? 0 : -1; }

void crypto_secure_wipe(void* buf, size_t len) { OPENSSL_cleanse(buf, len); }

int crypto_sha256(const uint8_t* data, size_t data_len, uint8_t* hash) {
    SHA256(data, data_len, hash);
    return 0;
}

uint32_t crypto_derive_address(const uint8_t* public_key) {
    uint8_t hash[32];
    crypto_sha256(public_key, BRAMBLE_KEY_SIZE, hash);
    return ((uint32_t)hash[0] << 24) | ((uint32_t)hash[1] << 16) | ((uint32_t)hash[2] << 8) |
           (uint32_t)hash[3];
}

uint32_t crypto_derive_pubkey_hash(const uint8_t* public_key) {
    uint8_t hash[32];
    crypto_sha256(public_key, BRAMBLE_KEY_SIZE, hash);
    return ((uint32_t)hash[4] << 24) | ((uint32_t)hash[5] << 16) | ((uint32_t)hash[6] << 8) |
           (uint32_t)hash[7];
}

int crypto_x25519_dh(const uint8_t* private_key, const uint8_t* peer_public_key,
                     uint8_t* shared_secret) {
    EVP_PKEY* priv = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key, 32);
    EVP_PKEY* pub = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer_public_key, 32);
    if (!priv || !pub) {
        EVP_PKEY_free(priv);
        EVP_PKEY_free(pub);
        return -1;
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(priv, NULL);
    int ret = -1;
    if (ctx && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, pub) == 1) {
        size_t len = 32;
        if (EVP_PKEY_derive(ctx, shared_secret, &len) == 1)
            ret = 0;
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(priv);
    EVP_PKEY_free(pub);
    if (ret == 0 && crypto_x25519_check_shared(shared_secret) != 0)
        ret = -1;
    return ret;
}

int crypto_hkdf_sha256(const uint8_t* salt, size_t salt_len, const uint8_t* ikm, size_t ikm_len,
                       const uint8_t* info, size_t info_len, uint8_t* okm, size_t okm_len) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    if (!ctx)
        return -1;
    int ret = -1;
    if (EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1 &&
        EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, (int)salt_len) == 1 &&
        EVP_PKEY_CTX_set1_hkdf_key(ctx, ikm, (int)ikm_len) == 1 &&
        EVP_PKEY_CTX_add1_hkdf_info(ctx, info, (int)info_len) == 1) {
        size_t len = okm_len;
        if (EVP_PKEY_derive(ctx, okm, &len) == 1)
            ret = 0;
    }
    EVP_PKEY_CTX_free(ctx);
    return ret;
}

int crypto_aes256gcm_encrypt(const uint8_t* key, const uint8_t* nonce, const uint8_t* plaintext,
                             size_t pt_len, const uint8_t* aad, size_t aad_len, uint8_t* ciphertext,
                             uint8_t* tag) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return -1;
    int ret = -1, len;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, BRAMBLE_NONCE_SIZE, NULL) == 1 &&
        EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) == 1) {
        if (aad && aad_len > 0)
            EVP_EncryptUpdate(ctx, NULL, &len, aad, (int)aad_len);
        if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, (int)pt_len) == 1 &&
            EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, BRAMBLE_TAG_SIZE, tag) == 1) {
            ret = 0;
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    return ret;
}

int crypto_aes256gcm_decrypt(const uint8_t* key, const uint8_t* nonce, const uint8_t* ciphertext,
                             size_t ct_len, const uint8_t* aad, size_t aad_len, const uint8_t* tag,
                             uint8_t* plaintext) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return -1;
    int ret = -1, len;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, BRAMBLE_NONCE_SIZE, NULL) == 1 &&
        EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) == 1) {
        if (aad && aad_len > 0)
            EVP_DecryptUpdate(ctx, NULL, &len, aad, (int)aad_len);
        if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, (int)ct_len) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, BRAMBLE_TAG_SIZE, (void*)tag) == 1 &&
            EVP_DecryptFinal_ex(ctx, plaintext + len, &len) == 1) {
            ret = 0;
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    return ret;
}

int crypto_hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len,
                       uint8_t* mac) {
    unsigned int len = 32;
    if (HMAC(EVP_sha256(), key, (int)key_len, data, data_len, mac, &len) == NULL)
        return -1;
    return 0;
}

int crypto_ed25519_keypair_from_seed(const uint8_t seed[32],
                                     uint8_t public_key[BRAMBLE_ED25519_PUBKEY_SIZE],
                                     uint8_t private_key[BRAMBLE_ED25519_SECKEY_SIZE]) {
    int ret = -1;
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    if (pkey) {
        size_t len = BRAMBLE_ED25519_PUBKEY_SIZE;
        if (EVP_PKEY_get_raw_public_key(pkey, public_key, &len) == 1 &&
            len == BRAMBLE_ED25519_PUBKEY_SIZE) {
            /* libsodium secret-key layout: seed || public key. */
            memcpy(private_key, seed, 32);
            memcpy(private_key + 32, public_key, 32);
            ret = 0;
        }
        EVP_PKEY_free(pkey);
    }
    return ret;
}

int crypto_ed25519_keypair(uint8_t public_key[BRAMBLE_ED25519_PUBKEY_SIZE],
                           uint8_t private_key[BRAMBLE_ED25519_SECKEY_SIZE]) {
    /* Seed from crypto_random() (mirrors the device path, where the same call
     * is the SEC-L1 entropy-gated source); fail closed on RNG failure. */
    uint8_t seed[32];
    if (crypto_random(seed, sizeof(seed)) != 0)
        return -1;

    int ret = crypto_ed25519_keypair_from_seed(seed, public_key, private_key);
    OPENSSL_cleanse(seed, sizeof(seed));
    return ret;
}

int crypto_ed25519_sign(const uint8_t private_key[BRAMBLE_ED25519_SECKEY_SIZE], const uint8_t* msg,
                        size_t msg_len, uint8_t sig[BRAMBLE_ED25519_SIG_SIZE]) {
    /* OpenSSL takes the 32-byte seed half of the libsodium-format key. */
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key, 32);
    if (!pkey)
        return -1;
    int ret = -1;
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    if (md && EVP_DigestSignInit(md, NULL, NULL, NULL, pkey) == 1) {
        size_t sig_len = BRAMBLE_ED25519_SIG_SIZE;
        if (EVP_DigestSign(md, sig, &sig_len, msg, msg_len) == 1 &&
            sig_len == BRAMBLE_ED25519_SIG_SIZE)
            ret = 0;
    }
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    return ret;
}

bool crypto_ed25519_verify(const uint8_t public_key[BRAMBLE_ED25519_PUBKEY_SIZE],
                           const uint8_t* msg, size_t msg_len,
                           const uint8_t sig[BRAMBLE_ED25519_SIG_SIZE]) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key,
                                                 BRAMBLE_ED25519_PUBKEY_SIZE);
    if (!pkey)
        return false;
    bool ok = false;
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    if (md && EVP_DigestVerifyInit(md, NULL, NULL, NULL, pkey) == 1)
        ok = EVP_DigestVerify(md, sig, BRAMBLE_ED25519_SIG_SIZE, msg, msg_len) == 1;
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    return ok;
}

int crypto_generate_identity(bramble_identity_t* id) {
    EVP_PKEY* pkey = NULL;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    if (!ctx)
        return -1;
    int ret = -1;
    /* Same build-aside-then-commit shape as the ESP backend: *id is untouched
     * on every failure path, so a partially built identity can never escape
     * into a caller (and regenerating over a live identity cannot corrupt it). */
    bramble_identity_t tmp;
    if (EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &pkey) == 1) {
        size_t len = 32;
        if (EVP_PKEY_get_raw_private_key(pkey, tmp.private_key, &len) == 1 &&
            EVP_PKEY_get_raw_public_key(pkey, tmp.public_key, &len) == 1 &&
            /* Ed25519 signing identity alongside X25519; fail closed (no
             * partial identity) if keygen fails. */
            crypto_ed25519_keypair(tmp.ed25519_public_key, tmp.ed25519_private_key) == 0) {
            /* The address (and pubkey_hash) derive from the
             * Ed25519 identity key, the key attestations are signed with,
             * so an address claim is only satisfiable by the keyholder. */
            tmp.address = crypto_derive_address(tmp.ed25519_public_key);
            tmp.pubkey_hash = crypto_derive_pubkey_hash(tmp.ed25519_public_key);
            /* The single commit point: nothing reaches *id before this line. */
            memcpy(id, &tmp, sizeof(tmp));
            ret = 0;
        }
    }
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    /* OPENSSL_cleanse-backed, not memset: tmp holds private key material and
     * the wipe of a dying local is exactly what a compiler may elide. */
    crypto_secure_wipe(&tmp, sizeof(tmp));
    return ret;
}

#endif /* ESP_PLATFORM */
