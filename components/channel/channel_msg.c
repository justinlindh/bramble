#include "include/channel_msg.h"
#include <string.h>

_Static_assert(CHANNEL_MSG_MAX_PLAINTEXT_SIZE >=
                   BRAMBLE_MAX_PACKET_SIZE - (BRAMBLE_NONCE_SIZE + BRAMBLE_TAG_SIZE),
               "plaintext buffer too small for max packet minus wire overhead");

int channel_msg_encrypt(const bramble_channel_t* ch, uint32_t src_addr, uint8_t app_type,
                        const uint8_t* data, size_t data_len, const uint8_t* aad, size_t aad_len,
                        uint8_t* nonce_out, uint8_t* ciphertext_out, uint8_t* tag_out) {
    if (!ch || !nonce_out || !ciphertext_out || !tag_out)
        return -1;

    /* Build inner plaintext: channel_id(1) + epoch(2) + app_type(1) + src_addr(4) + data */
    size_t pt_len = CHANNEL_MSG_OVERHEAD + data_len;
    uint8_t pt[CHANNEL_MSG_MAX_PLAINTEXT_SIZE];
    if (pt_len > sizeof(pt))
        return -1;

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

    /* Encrypt with header AAD binding */
    return crypto_aes256gcm_encrypt(ch->key, nonce_out, pt, pt_len, aad, aad_len, ciphertext_out,
                                    tag_out);
}

/* Try decrypting with a single key. Returns 0 on success. */
static int try_decrypt(const uint8_t* key, const uint8_t* nonce, const uint8_t* ciphertext,
                       size_t ct_len, const uint8_t* tag, const uint8_t* aad, size_t aad_len,
                       uint8_t* plaintext_out) {
    return crypto_aes256gcm_decrypt(key, nonce, ciphertext, ct_len, aad, aad_len, tag,
                                    plaintext_out);
}

int channel_msg_decrypt(bramble_channel_t* channels, int num_channels, const uint8_t* nonce,
                        const uint8_t* ciphertext, size_t ct_len, const uint8_t* tag,
                        const uint8_t* aad, size_t aad_len, uint8_t* plaintext_out,
                        channel_msg_info_t* info_out) {
    if (!channels || !nonce || !ciphertext || !tag || !plaintext_out || !info_out)
        return -1;
    if (ct_len < CHANNEL_MSG_OVERHEAD)
        return -1;

    uint8_t pt[CHANNEL_MSG_MAX_PLAINTEXT_SIZE];
    if (ct_len > sizeof(pt))
        return -1;

    /* Constant-time trial decryption: always try ALL channels to prevent
       timing side-channels. Store first successful result. */
    int found_index = -1;
    channel_msg_info_t found_info;
    memset(&found_info, 0, sizeof(found_info));

    /* We need to save/restore channel states for epoch catch-up,
       so track which channel was successfully caught up */
    int catchup_index = -1;
    uint8_t catchup_key[BRAMBLE_KEY_SIZE];
    uint16_t catchup_epoch = 0;

    int limit = num_channels < MAX_CHANNELS ? num_channels : MAX_CHANNELS;

    for (int i = 0; i < limit; i++) {
        /* Try with current key */
        if (try_decrypt(channels[i].key, nonce, ciphertext, ct_len, tag, aad, aad_len, pt) == 0) {
            if (found_index < 0) {
                found_index = i;
                found_info.channel_id = pt[0];
                found_info.epoch = (uint16_t)(pt[1] | (pt[2] << 8));
                found_info.app_type = pt[3];
                found_info.src_addr =
                    (uint32_t)(pt[4] | (pt[5] << 8) | (pt[6] << 16) | (pt[7] << 24));
                found_info.data = NULL;
                found_info.data_len = ct_len - CHANNEL_MSG_OVERHEAD;
                found_info.channel_index = i;
            }
            /* Do NOT break — continue trying all channels for constant time */
            continue;
        }

        /* Epoch catch-up: try advancing up to 256 times */
        uint8_t saved_key[BRAMBLE_KEY_SIZE];
        uint16_t saved_epoch = channels[i].epoch;
        memcpy(saved_key, channels[i].key, BRAMBLE_KEY_SIZE);

        bool caught_up = false;
        for (int j = 0; j < CHANNEL_EPOCH_CATCHUP_MAX; j++) {
            if (channel_advance_epoch(&channels[i]) != 0)
                break;
            if (try_decrypt(channels[i].key, nonce, ciphertext, ct_len, tag, aad, aad_len, pt) ==
                0) {
                caught_up = true;
                break;
            }
        }

        if (caught_up && found_index < 0) {
            found_index = i;
            found_info.channel_id = pt[0];
            found_info.epoch = (uint16_t)(pt[1] | (pt[2] << 8));
            found_info.app_type = pt[3];
            found_info.src_addr = (uint32_t)(pt[4] | (pt[5] << 8) | (pt[6] << 16) | (pt[7] << 24));
            found_info.data_len = ct_len - CHANNEL_MSG_OVERHEAD;
            found_info.data = NULL;
            found_info.channel_index = i;
            /* Save the caught-up state to apply later */
            catchup_index = i;
            memcpy(catchup_key, channels[i].key, BRAMBLE_KEY_SIZE);
            catchup_epoch = channels[i].epoch;
        }

        /* Restore original key state (we'll re-apply catchup for the winner after the loop) */
        memcpy(channels[i].key, saved_key, BRAMBLE_KEY_SIZE);
        channels[i].epoch = saved_epoch;
    }

    if (found_index < 0)
        return -1;

    /* Apply the caught-up epoch state for the matching channel if needed */
    if (catchup_index >= 0 && catchup_index == found_index) {
        memcpy(channels[catchup_index].key, catchup_key, BRAMBLE_KEY_SIZE);
        channels[catchup_index].epoch = catchup_epoch;
    }

    /* Re-decrypt the winning channel to recover plaintext data.
       The constant-time loop above may have overwritten pt with later attempts.
       We need to decrypt one more time to get the actual plaintext. */
    if (try_decrypt(channels[found_index].key, nonce, ciphertext, ct_len, tag, aad, aad_len, pt) !=
        0) {
        return -1;
    }

    if (ct_len > CHANNEL_MSG_OVERHEAD) {
        memcpy(plaintext_out, pt + CHANNEL_MSG_OVERHEAD, ct_len - CHANNEL_MSG_OVERHEAD);
    }
    found_info.data = plaintext_out;

    *info_out = found_info;
    return 0;
}
