#ifndef BRAMBLE_PUBLIC_CHANNEL_H
#define BRAMBLE_PUBLIC_CHANNEL_H

#include "channel_key.h"
#include <stdbool.h>
#include <stdint.h>

#define BRAMBLE_PUBLIC_CHANNEL_INDEX 0
#define BRAMBLE_PUBLIC_CHANNEL_NAME "Bramble Common"
#define BRAMBLE_PUBLIC_CHANNEL_HOP_LIMIT 3
#define BRAMBLE_PUBLIC_CHANNEL_RATE_LIMIT_MS 30000
#define BRAMBLE_PUBLIC_CHANNEL_BURST 3

/* Initialize Channel 0 with well-known PSK. Call on boot. */
int public_channel_init(bramble_channel_t* channels, int* num_channels);

/* TX rate limiter — returns true if allowed to send, false if rate-limited */
bool public_channel_can_send(uint32_t now_ms);

/* RX rate limiter per source — returns true if this source is within limits */
bool public_channel_rx_check(uint32_t src_addr, uint32_t now_ms);

#endif
