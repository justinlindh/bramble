// ESP32 crypto implementation using mbedtls (hardware-accelerated on ESP32-S3)
#ifdef ESP_PLATFORM

#include "crypto.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/platform_util.h"
#include "esp_random.h"
#include "esp_log.h"
#include "crypto_entropy.h"
#include "sodium.h"
#include <string.h>

/* RNG callback for mbedtls_ecp_mul (required for side-channel blinding) */
static int crypto_rng_callback(void* ctx, unsigned char* buf, size_t len) {
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

int crypto_sha256(const uint8_t* data, size_t data_len, uint8_t* hash) {
    mbedtls_sha256(data, data_len, hash, 0);
    return 0;
}

uint32_t crypto_derive_address(const uint8_t* public_key) {
    uint8_t hash[32];
    crypto_sha256(public_key, 32, hash);
    return ((uint32_t)hash[0] << 24) | ((uint32_t)hash[1] << 16) | ((uint32_t)hash[2] << 8) |
           (uint32_t)hash[3];
}

uint32_t crypto_derive_pubkey_hash(const uint8_t* public_key) {
    /* SHA256(pub)[4:8]: an independent slice, distinct from the address
     * (SHA256(pub)[0:4]), so identity_check_collision can tell two keys with
     * the same derived address apart. MUST match crypto_host.c; the exact
     * bytes are pinned by test_identity.c. Historically this returned
     * crypto_derive_address(), which made the collision check a no-op on
     * device. */
    uint8_t hash[32];
    crypto_sha256(public_key, 32, hash);
    return ((uint32_t)hash[4] << 24) | ((uint32_t)hash[5] << 16) | ((uint32_t)hash[6] << 8) |
           (uint32_t)hash[7];
}

int crypto_aes256gcm_encrypt(const uint8_t* key, const uint8_t* nonce, const uint8_t* plaintext,
                             size_t pt_len, const uint8_t* aad, size_t aad_len, uint8_t* ciphertext,
                             uint8_t* tag) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    int ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, pt_len, nonce, 12, aad, aad_len,
                                        plaintext, ciphertext, 16, tag);
    mbedtls_gcm_free(&ctx);
    return (ret == 0) ? 0 : -1;
}

int crypto_aes256gcm_decrypt(const uint8_t* key, const uint8_t* nonce, const uint8_t* ciphertext,
                             size_t ct_len, const uint8_t* aad, size_t aad_len, const uint8_t* tag,
                             uint8_t* plaintext) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    int ret = mbedtls_gcm_auth_decrypt(&ctx, ct_len, nonce, 12, aad, aad_len, tag, 16, ciphertext,
                                       plaintext);
    mbedtls_gcm_free(&ctx);
    return (ret == 0) ? 0 : -1;
}

int crypto_hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len,
                       uint8_t* mac) {
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return mbedtls_md_hmac(md_info, key, key_len, data, data_len, mac);
}

int crypto_hkdf_sha256(const uint8_t* salt, size_t salt_len, const uint8_t* ikm, size_t ikm_len,
                       const uint8_t* info, size_t info_len, uint8_t* okm, size_t okm_len) {
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return mbedtls_hkdf(md_info, salt, salt_len, ikm, ikm_len, info, info_len, okm, okm_len);
}

int crypto_random(uint8_t* buf, size_t len) {
    /* Fail closed behind the entropy gate; zeroes buf and returns -1 when the
     * gate is shut (SEC-L1). See crypto_entropy.c. */
    return crypto_entropy_fill(buf, len, esp_random);
}

void crypto_secure_wipe(void* buf, size_t len) { mbedtls_platform_zeroize(buf, len); }

int crypto_x25519_dh(const uint8_t* private_key, const uint8_t* peer_public_key,
                     uint8_t* shared_secret) {
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

    if (mbedtls_ecp_mul(&grp, &R, &d, &Qp, crypto_rng_callback, NULL) == 0) {
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
    /* RFC 7748 contributory check: mbedtls does not reject a low-order peer
     * point on its own, so a malicious/degenerate peer_public_key can force
     * an all-zero shared_secret here. Reject it explicitly. */
    if (ret == 0 && crypto_x25519_check_shared(shared_secret) != 0)
        ret = -1;
    return ret;
}

/* Ed25519 via libsodium (espressif/libsodium managed component); ESP-IDF
 * mbedtls has no Ed25519. sodium_init() is idempotent (returns 1 when
 * already initialized) and only fails on catastrophic misconfiguration. */
int crypto_ed25519_keypair_from_seed(const uint8_t seed[32],
                                     uint8_t public_key[BRAMBLE_ED25519_PUBKEY_SIZE],
                                     uint8_t private_key[BRAMBLE_ED25519_SECKEY_SIZE]) {
    if (sodium_init() < 0)
        return -1;
    return (crypto_sign_seed_keypair(public_key, private_key, seed) == 0) ? 0 : -1;
}

int crypto_ed25519_keypair(uint8_t public_key[BRAMBLE_ED25519_PUBKEY_SIZE],
                           uint8_t private_key[BRAMBLE_ED25519_SECKEY_SIZE]) {
    /* Seed from crypto_random(): the SEC-L1 entropy-gated source
     * (crypto_entropy_fill + esp_random, see crypto_random() above). Draw
     * into a scratch buffer and fail closed before touching the caller's
     * key buffers, mirroring crypto_generate_identity(). */
    uint8_t seed[32];
    if (crypto_random(seed, sizeof(seed)) != 0) {
        /* Entropy gate shut: refuse rather than derive from a zeroed seed. */
        return -1;
    }
    int ret = crypto_ed25519_keypair_from_seed(seed, public_key, private_key);
    mbedtls_platform_zeroize(seed, sizeof(seed));
    return ret;
}

int crypto_ed25519_sign(const uint8_t private_key[BRAMBLE_ED25519_SECKEY_SIZE], const uint8_t* msg,
                        size_t msg_len, uint8_t sig[BRAMBLE_ED25519_SIG_SIZE]) {
    if (sodium_init() < 0)
        return -1;
    return (crypto_sign_detached(sig, NULL, msg, msg_len, private_key) == 0) ? 0 : -1;
}

bool crypto_ed25519_verify(const uint8_t public_key[BRAMBLE_ED25519_PUBKEY_SIZE],
                           const uint8_t* msg, size_t msg_len,
                           const uint8_t sig[BRAMBLE_ED25519_SIG_SIZE]) {
    if (sodium_init() < 0)
        return false;
    return crypto_sign_verify_detached(sig, msg, msg_len, public_key) == 0;
}

int crypto_generate_identity(bramble_identity_t* id) {
    /* Draw into a scratch buffer first: crypto_random zeroes its destination
     * in place when the entropy gate is shut, so drawing straight into
     * id->private_key would clobber a caller's existing identity even on
     * failure (SEC-L1). Only commit into id once the draw has succeeded. */
    uint8_t priv[32];
    if (crypto_random(priv, sizeof(priv)) != 0) {
        /* Entropy gate shut: refuse rather than clamp-and-use a zeroed key. */
        return -1;
    }
    memcpy(id->private_key, priv, sizeof(priv));
    /* Wipe the stack scratch now that the key has been committed to id.
     * mbedtls_platform_zeroize (not memset) so the compiler cannot optimize
     * the wipe away as a dead store to a about-to-go-out-of-scope buffer. */
    mbedtls_platform_zeroize(priv, sizeof(priv));
    // Clamp per X25519 spec
    id->private_key[0] &= 248;
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

    /* RNG callback required for side-channel blinding on ESP-IDF mbedtls */
    int ok = (mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, crypto_rng_callback, NULL) == 0) &&
             (mbedtls_mpi_write_binary_le(&Q.MBEDTLS_PRIVATE(X), id->public_key, 32) == 0);

    /* Ed25519 signing identity alongside X25519 (Phase 1). Fail closed: if
     * keygen fails (e.g. entropy gate shut), propagate failure rather than
     * hand back a partial identity. */
    if (ok && crypto_ed25519_keypair(id->ed25519_public_key, id->ed25519_private_key) != 0)
        ok = 0;

    if (ok) {
        /* Phase 4 rebind: the address (and pubkey_hash) derive from the
         * Ed25519 identity key, the key attestations are signed with, so an
         * address claim is only satisfiable by the keyholder. */
        id->address = crypto_derive_address(id->ed25519_public_key);
        id->pubkey_hash = crypto_derive_pubkey_hash(id->ed25519_public_key);
    }

    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    return ok ? 0 : -1;
}

#endif // ESP_PLATFORM
