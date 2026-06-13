#ifndef BRAMBLE_PUBLIC_CHANNEL_H
#define BRAMBLE_PUBLIC_CHANNEL_H

#include "channel_key.h"
#include <stdint.h>

#define BRAMBLE_PUBLIC_CHANNEL_INDEX 0
#define BRAMBLE_PUBLIC_CHANNEL_NAME "Bramble Common"
#define BRAMBLE_PUBLIC_CHANNEL_HOP_LIMIT 3

/* Initialize Channel 0 with well-known PSK. Call on boot. */
int public_channel_init(bramble_channel_t* channels, int* num_channels);

#endif
