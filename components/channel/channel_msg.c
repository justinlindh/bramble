#include "include/channel_msg.h"
#include <string.h>

_Static_assert(CHANNEL_MSG_MAX_PLAINTEXT_SIZE >=
                   BRAMBLE_MAX_PACKET_SIZE - (BRAMBLE_NONCE_SIZE + BRAMBLE_TAG_SIZE),
               "plaintext buffer too small for max packet minus wire overhead");

int channel_msg_encrypt(const bramble_channel_t* ch, uint32_t src_addr, uint8_t app_type,
                        uint32_t sent_at, const uint8_t* data, size_t data_len, const uint8_t* aad,
                        size_t aad_len, const uint8_t* nonce_in, uint8_t* ciphertext_out,
                        uint8_t* tag_out) {
    if (!ch || !nonce_in || !ciphertext_out || !tag_out)
        return -1;

    /* Build inner plaintext: channel_id(1) + epoch(2) + app_type(1) + src_addr(4)
     * + [sent_at(4), CHAT only] + data. sent_at rides inside the GCM
     * plaintext (not the cleartext header) so it is authenticated: nobody
     * without the channel key can forge or alter it. */
    bool has_sent_at = (app_type == APP_TYPE_CHAT);
    size_t hdr_len = CHANNEL_MSG_OVERHEAD + (has_sent_at ? CHANNEL_MSG_SENT_AT_SIZE : 0);
    size_t pt_len = hdr_len + data_len;
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
    if (has_sent_at) {
        pt[8] = (uint8_t)(sent_at >> 24);
        pt[9] = (uint8_t)(sent_at >> 16);
        pt[10] = (uint8_t)(sent_at >> 8);
        pt[11] = (uint8_t)(sent_at);
    }
    if (data_len > 0 && data) {
        memcpy(pt + hdr_len, data, data_len);
    }

    /* Nonce is caller-supplied (nonce_counter, SEC-C reuse-avoidance): the
     * deterministic node-global counter guarantees no two encryptions under
     * the same channel key ever reuse a nonce, which random generation alone
     * cannot (birthday bound over a node's lifetime). Nonce reuse under one
     * channel key is catastrophic (keystream XOR leak, auth-key
     * recovery/forgery). */
    return crypto_aes256gcm_encrypt(ch->key, nonce_in, pt, pt_len, aad, aad_len, ciphertext_out,
                                    tag_out);
}

/* Per-channel epoch catch-up budget (SEC-I1; rationale in channel_msg.h).
 * Indexed by position in the channels array. */
typedef struct {
    uint32_t tokens;
    uint32_t last_refill_ms;
    bool initialized;
} catchup_bucket_t;

static catchup_bucket_t s_catchup[MAX_CHANNELS];

void channel_msg_catchup_reset(void) { memset(s_catchup, 0, sizeof(s_catchup)); }

/* Refill and return the bucket for channel index i. */
static catchup_bucket_t* catchup_bucket(int i, uint32_t now_ms) {
    catchup_bucket_t* b = &s_catchup[i];
    if (!b->initialized) {
        b->tokens = CHANNEL_EPOCH_CATCHUP_BUDGET;
        b->last_refill_ms = now_ms;
        b->initialized = true;
        return b;
    }
    /* Unsigned subtraction is wrap-safe across the 49.7-day rollover */
    uint32_t elapsed = now_ms - b->last_refill_ms;
    uint32_t add = (uint32_t)(((uint64_t)elapsed * CHANNEL_EPOCH_CATCHUP_BUDGET) /
                              CHANNEL_EPOCH_CATCHUP_REFILL_MS);
    if (add > 0) {
        uint32_t headroom = CHANNEL_EPOCH_CATCHUP_BUDGET - b->tokens;
        b->tokens += (add < headroom) ? add : headroom;
        b->last_refill_ms = now_ms;
    }
    return b;
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
                        channel_msg_info_t* info_out, uint32_t now_ms) {
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
            /* Do NOT break, continue trying all channels for constant time */
            continue;
        }

        /* Epoch catch-up: try advancing up to 256 times, bounded by the
         * per-channel rate budget (SEC-I1) */
        uint8_t saved_key[BRAMBLE_KEY_SIZE];
        uint16_t saved_epoch = channels[i].epoch;
        memcpy(saved_key, channels[i].key, BRAMBLE_KEY_SIZE);

        catchup_bucket_t* budget = catchup_bucket(i, now_ms);
        bool caught_up = false;
        uint32_t consumed = 0;
        for (int j = 0; j < CHANNEL_EPOCH_CATCHUP_MAX; j++) {
            if (budget->tokens == 0)
                break;
            budget->tokens--;
            consumed++;
            if (channel_advance_epoch(&channels[i]) != 0)
                break;
            if (try_decrypt(channels[i].key, nonce, ciphertext, ct_len, tag, aad, aad_len, pt) ==
                0) {
                caught_up = true;
                break;
            }
        }
        if (caught_up) {
            /* Refund successful recoveries: a catch-up that lands on a
             * valid ciphertext is legitimate by definition (forging one
             * requires the channel key), so only FAILED catch-up work,
             * attacker garbage and cross-channel misses, is charged.
             * Legitimate deep-drift recovery therefore never drains its
             * own bucket. */
            uint32_t headroom = CHANNEL_EPOCH_CATCHUP_BUDGET - budget->tokens;
            budget->tokens += (consumed < headroom) ? consumed : headroom;
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

    /* app_type (pt[3]) is authenticated by the tag we just verified, so it is
     * safe to trust for deciding whether a sent_at field follows. Guard
     * ct_len explicitly: app_type is attacker-choosable by any legitimate
     * channel-key holder, so a malformed CHAT message shorter than the
     * sent_at field it claims to carry must not be read out of bounds; treat
     * it as carrying no sent_at rather than misparsing past the buffer. */
    size_t hdr_len = CHANNEL_MSG_OVERHEAD;
    if (pt[3] == APP_TYPE_CHAT && ct_len >= CHANNEL_MSG_OVERHEAD + CHANNEL_MSG_SENT_AT_SIZE) {
        found_info.sent_at = ((uint32_t)pt[8] << 24) | ((uint32_t)pt[9] << 16) |
                             ((uint32_t)pt[10] << 8) | (uint32_t)pt[11];
        hdr_len += CHANNEL_MSG_SENT_AT_SIZE;
    } else {
        found_info.sent_at = 0;
    }

    if (ct_len > hdr_len) {
        memcpy(plaintext_out, pt + hdr_len, ct_len - hdr_len);
    }
    found_info.data_len = ct_len > hdr_len ? ct_len - hdr_len : 0;
    found_info.data = plaintext_out;

    *info_out = found_info;
    return 0;
}
