#ifndef BRAMBLE_CHANNEL_MSG_H
#define BRAMBLE_CHANNEL_MSG_H

#include "channel_key.h"
#include "crypto.h"

#define CHANNEL_MSG_OVERHEAD 8  /* channel_id(1) + epoch(2) + app_type(1) + src_addr(4) */
#define MAX_CHANNELS 16

int channel_msg_encrypt(const bramble_channel_t *ch, uint32_t src_addr, uint8_t app_type,
                        const uint8_t *data, size_t data_len,
                        uint8_t *nonce_out, uint8_t *ciphertext_out, uint8_t *tag_out);

typedef struct {
    uint8_t channel_id;
    uint16_t epoch;
    uint8_t app_type;
    uint32_t src_addr;
    const uint8_t *data;
    size_t data_len;
    int channel_index;
} channel_msg_info_t;

int channel_msg_decrypt(bramble_channel_t *channels, int num_channels,
                        const uint8_t *nonce, const uint8_t *ciphertext, size_t ct_len,
                        const uint8_t *tag, channel_msg_info_t *info_out);

#endif
