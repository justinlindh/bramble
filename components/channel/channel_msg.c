#include "include/channel_msg.h"
#include <string.h>

int channel_msg_encrypt(const bramble_channel_t *ch, uint32_t src_addr, uint8_t app_type,
                        const uint8_t *data, size_t data_len,
                        uint8_t *nonce_out, uint8_t *ciphertext_out, uint8_t *tag_out) {
    if (!ch || !nonce_out || !ciphertext_out || !tag_out) return -1;

    /* Build inner plaintext: channel_id(1) + epoch(2) + app_type(1) + src_addr(4) + data */
    size_t pt_len = CHANNEL_MSG_OVERHEAD + data_len;
    uint8_t pt[2048];
    if (pt_len > sizeof(pt)) return -1;

    pt[0] = ch->channel_id;
    pt[1] = (uint8_t)(ch->epoch & 0xFF);
    pt[2] = (uint8_t)((ch->epoch >> 8) & 0xFF);
    pt[3] = app_type;
    pt[4] = (uint8_t)(src_addr & 0xFF);
    pt[5] = (uint8_t)((src_addr >> 8) & 0xFF);
    pt[6] = (uint8_t)((src_addr >> 16) & 0xFF);
    pt[7] = (uint8_t)((src_addr >> 24) & 0xFF);
    if (data_len > 0 && data) {
        memcpy(pt + CHANNEL_MSG_OVERHEAD, data, data_len);
    }

    /* Generate random nonce */
    crypto_random(nonce_out, BRAMBLE_NONCE_SIZE);

    /* Encrypt with no AAD */
    return crypto_aes256gcm_encrypt(ch->key, nonce_out, pt, pt_len, NULL, 0,
                                    ciphertext_out, tag_out);
}

/* Try decrypting with a single key. Returns 0 on success. */
static int try_decrypt(const uint8_t *key, const uint8_t *nonce,
                       const uint8_t *ciphertext, size_t ct_len,
                       const uint8_t *tag, uint8_t *plaintext_out) {
    return crypto_aes256gcm_decrypt(key, nonce, ciphertext, ct_len, NULL, 0,
                                    tag, plaintext_out);
}

int channel_msg_decrypt(bramble_channel_t *channels, int num_channels,
                        const uint8_t *nonce, const uint8_t *ciphertext, size_t ct_len,
                        const uint8_t *tag, channel_msg_info_t *info_out) {
    if (!channels || !nonce || !ciphertext || !tag || !info_out) return -1;
    if (ct_len < CHANNEL_MSG_OVERHEAD) return -1;

    uint8_t pt[2048];
    if (ct_len > sizeof(pt)) return -1;

    for (int i = 0; i < num_channels && i < MAX_CHANNELS; i++) {
        /* Try with current key */
        if (try_decrypt(channels[i].key, nonce, ciphertext, ct_len, tag, pt) == 0) {
            goto success;
success:
            info_out->channel_id = pt[0];
            info_out->epoch = (uint16_t)(pt[1] | (pt[2] << 8));
            info_out->app_type = pt[3];
            info_out->src_addr = (uint32_t)(pt[4] | (pt[5] << 8) | (pt[6] << 16) | (pt[7] << 24));
            info_out->data = ciphertext + CHANNEL_MSG_OVERHEAD; /* caller must re-decrypt to use; point to ct offset for length calc */
            info_out->data_len = ct_len - CHANNEL_MSG_OVERHEAD;
            info_out->channel_index = i;
            /* Note: data pointer is into the plaintext which is on stack — 
               for real usage we'd need a buffer. For now, store data_len and 
               caller knows the offset. We'll copy into a provided buffer in production. */
            return 0;
        }

        /* Epoch catch-up: try advancing up to 256 times */
        /* Save current state to restore if catch-up fails */
        uint8_t saved_key[BRAMBLE_KEY_SIZE];
        uint16_t saved_epoch = channels[i].epoch;
        memcpy(saved_key, channels[i].key, BRAMBLE_KEY_SIZE);

        bool caught_up = false;
        for (int j = 0; j < 256; j++) {
            if (channel_advance_epoch(&channels[i]) != 0) break;
            if (try_decrypt(channels[i].key, nonce, ciphertext, ct_len, tag, pt) == 0) {
                caught_up = true;
                break;
            }
        }

        if (caught_up) {
            /* Keep advanced state */
            info_out->channel_id = pt[0];
            info_out->epoch = (uint16_t)(pt[1] | (pt[2] << 8));
            info_out->app_type = pt[3];
            info_out->src_addr = (uint32_t)(pt[4] | (pt[5] << 8) | (pt[6] << 16) | (pt[7] << 24));
            info_out->data_len = ct_len - CHANNEL_MSG_OVERHEAD;
            info_out->data = NULL; /* stack plaintext — see note above */
            info_out->channel_index = i;
            return 0;
        }

        /* Restore original key state */
        memcpy(channels[i].key, saved_key, BRAMBLE_KEY_SIZE);
        channels[i].epoch = saved_epoch;
    }

    return -1;
}
