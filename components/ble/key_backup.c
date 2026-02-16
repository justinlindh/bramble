#include "key_backup.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_random.h"
#else
#include <openssl/rand.h>
#include <openssl/evp.h>
#endif

#define NONCE_SIZE 12
#define TAG_SIZE   16
#define BACKUP_BLOB_SIZE (NONCE_SIZE + BACKUP_KEY_MATERIAL_SIZE + TAG_SIZE) // 96

void key_backup_init(key_backup_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = BACKUP_IDLE;
}

int key_backup_request(key_backup_ctx_t *ctx, uint32_t now_ms) {
    if (ctx->state != BACKUP_IDLE && ctx->state != BACKUP_TIMEOUT &&
        ctx->state != BACKUP_COMPLETE) {
        return -1;
    }
    ctx->state = BACKUP_REQUESTED;
    ctx->request_time = now_ms;
    ctx->button_pressed = false;
    return 0;
}

int key_backup_authorize(key_backup_ctx_t *ctx, uint32_t now_ms) {
    (void)now_ms;
    if (ctx->state != BACKUP_REQUESTED) {
        return -1;
    }
    ctx->state = BACKUP_AUTHORIZED;
    ctx->button_pressed = true;
    return 0;
}

void key_backup_tick(key_backup_ctx_t *ctx, uint32_t now_ms) {
    if (ctx->state == BACKUP_REQUESTED) {
        if ((now_ms - ctx->request_time) >= BACKUP_AUTH_TIMEOUT_MS) {
            ctx->state = BACKUP_TIMEOUT;
        }
    }
}

backup_state_t key_backup_get_state(const key_backup_ctx_t *ctx) {
    return ctx->state;
}

int key_backup_export(key_backup_ctx_t *ctx, const uint8_t *identity_privkey,
                      const uint8_t *identity_pubkey, uint32_t node_addr,
                      const uint8_t *encryption_key,
                      uint8_t *out, size_t out_max, size_t *out_len) {
    if (ctx->state != BACKUP_AUTHORIZED) return -1;
    if (out_max < BACKUP_BLOB_SIZE) return -1;

    // Build plaintext: privkey(32) + pubkey(32) + addr(4)
    uint8_t plaintext[BACKUP_KEY_MATERIAL_SIZE];
    memcpy(plaintext, identity_privkey, 32);
    memcpy(plaintext + 32, identity_pubkey, 32);
    memcpy(plaintext + 64, &node_addr, 4);

    // Generate random nonce
    uint8_t *nonce = out; // first 12 bytes of output
    RAND_bytes(nonce, NONCE_SIZE);

    // Encrypt with AES-256-GCM
    uint8_t *ciphertext = out + NONCE_SIZE;
    uint8_t *tag = out + NONCE_SIZE + BACKUP_KEY_MATERIAL_SIZE;

    EVP_CIPHER_CTX *evp_ctx = EVP_CIPHER_CTX_new();
    if (!evp_ctx) return -1;

    int ret = -1;
    int outl = 0;
    if (EVP_EncryptInit_ex(evp_ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto cleanup;
    if (EVP_EncryptInit_ex(evp_ctx, NULL, NULL, encryption_key, nonce) != 1) goto cleanup;
    if (EVP_EncryptUpdate(evp_ctx, ciphertext, &outl, plaintext, BACKUP_KEY_MATERIAL_SIZE) != 1) goto cleanup;
    if (EVP_EncryptFinal_ex(evp_ctx, ciphertext + outl, &outl) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(evp_ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag) != 1) goto cleanup;

    ctx->state = BACKUP_SENDING;
    *out_len = BACKUP_BLOB_SIZE;
    ret = 0;

cleanup:
    EVP_CIPHER_CTX_free(evp_ctx);
    return ret;
}

int key_backup_import(const uint8_t *encrypted_backup, size_t backup_len,
                      const uint8_t *encryption_key,
                      uint8_t *privkey_out, uint8_t *pubkey_out, uint32_t *addr_out) {
    if (backup_len != BACKUP_BLOB_SIZE) return -1;

    const uint8_t *nonce = encrypted_backup;
    const uint8_t *ciphertext = encrypted_backup + NONCE_SIZE;
    const uint8_t *tag = encrypted_backup + NONCE_SIZE + BACKUP_KEY_MATERIAL_SIZE;

    uint8_t plaintext[BACKUP_KEY_MATERIAL_SIZE];

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int ret = -1;
    int outl = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto cleanup;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, encryption_key, nonce) != 1) goto cleanup;
    if (EVP_DecryptUpdate(ctx, plaintext, &outl, ciphertext, BACKUP_KEY_MATERIAL_SIZE) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE, (void *)tag) != 1) goto cleanup;
    if (EVP_DecryptFinal_ex(ctx, plaintext + outl, &outl) != 1) goto cleanup;

    memcpy(privkey_out, plaintext, 32);
    memcpy(pubkey_out, plaintext + 32, 32);
    memcpy(addr_out, plaintext + 64, 4);
    ret = 0;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return ret;
}
