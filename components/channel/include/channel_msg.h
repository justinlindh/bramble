#ifndef BRAMBLE_CHANNEL_MSG_H
#define BRAMBLE_CHANNEL_MSG_H

#include "channel_key.h"
#include "crypto.h"
#include "packet.h"

#define CHANNEL_MSG_OVERHEAD 8 /* channel_id(1) + epoch(2) + app_type(1) + src_addr(4) */
#define MAX_CHANNELS 16

/*
 * Max plaintext buffer size for channel encrypt/decrypt.
 * Must be >= BRAMBLE_MAX_PACKET_SIZE minus channel wire overhead (nonce + tag).
 * 256 provides margin over the ~173 byte practical max.
 */
#define CHANNEL_MSG_MAX_PLAINTEXT_SIZE 256

/*
 * Max epoch drift to attempt during trial decryption.
 * 256 covers the full uint8_t rollover range.
 */
#define CHANNEL_EPOCH_CATCHUP_MAX 256

/*
 * Epoch catch-up rate limit (SEC-I1).
 *
 * Without a cap, every received packet that fails current-key decryption
 * burns up to CHANNEL_EPOCH_CATCHUP_MAX HKDF+GCM attempts on EVERY
 * non-matching channel (worst case 16 x 256 = 4096 per packet), which an
 * attacker triggers with garbage and which legitimate cross-channel
 * traffic triggers as a side effect.
 *
 * Each channel gets a token bucket: capacity equal to one full worst-case
 * recovery (256 attempts), refilled at one full budget per 10 seconds.
 * Why this cannot break legitimate recovery: catch-up is a ONE-TIME cost
 * per drift event (after success the channel state is advanced and
 * subsequent packets cost zero), routine rekeys drift by 1-2 epochs, and
 * even a node returning from a 256-epoch absence recovers on the first
 * packet with a full bucket, or within 10 seconds of sustained traffic
 * draining it. Sustained abuse is capped at ~26 GCM attempts/s/channel
 * instead of thousands per second.
 */
#define CHANNEL_EPOCH_CATCHUP_BUDGET CHANNEL_EPOCH_CATCHUP_MAX
#define CHANNEL_EPOCH_CATCHUP_REFILL_MS 10000

/* Reset all catch-up buckets to full (tests and channel reconfiguration). */
void channel_msg_catchup_reset(void);

int channel_msg_encrypt(const bramble_channel_t* ch, uint32_t src_addr, uint8_t app_type,
                        const uint8_t* data, size_t data_len, const uint8_t* aad, size_t aad_len,
                        uint8_t* nonce_out, uint8_t* ciphertext_out, uint8_t* tag_out);

typedef struct {
    uint8_t channel_id;
    uint16_t epoch;
    uint8_t app_type;
    uint32_t src_addr;
    const uint8_t* data;
    size_t data_len;
    int channel_index;
} channel_msg_info_t;

/* now_ms: monotonic milliseconds, drives the epoch catch-up rate limiter
 * (wrap-safe). Callers on device pass esp_timer-derived time. */
int channel_msg_decrypt(bramble_channel_t* channels, int num_channels, const uint8_t* nonce,
                        const uint8_t* ciphertext, size_t ct_len, const uint8_t* tag,
                        const uint8_t* aad, size_t aad_len, uint8_t* plaintext_out,
                        channel_msg_info_t* info_out, uint32_t now_ms);

#endif
