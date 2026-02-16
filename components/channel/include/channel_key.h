#ifndef BRAMBLE_CHANNEL_KEY_H
#define BRAMBLE_CHANNEL_KEY_H

#include "crypto.h"

typedef struct {
    uint8_t key[BRAMBLE_KEY_SIZE];
    uint8_t channel_id;
    uint16_t epoch;
} bramble_channel_t;

int channel_derive_key(const char *psk, bramble_channel_t *ch);
int channel_advance_epoch(bramble_channel_t *ch);

#endif
