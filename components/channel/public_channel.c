#include "include/public_channel.h"
#include <string.h>

int public_channel_init(bramble_channel_t *channels, int *num_channels) {
    if (!channels || !num_channels) return -1;
    int ret = channel_derive_key(BRAMBLE_PUBLIC_CHANNEL_PSK, &channels[BRAMBLE_PUBLIC_CHANNEL_INDEX]);
    if (ret != 0) return ret;
    /* Public channel ID is protocol-fixed at index 0 (don't use hash-derived channel_id). */
    channels[BRAMBLE_PUBLIC_CHANNEL_INDEX].channel_id = BRAMBLE_PUBLIC_CHANNEL_INDEX;
    if (*num_channels < 1) *num_channels = 1;
    return 0;
}

/* TX token bucket state */
static uint32_t tx_tokens = BRAMBLE_PUBLIC_CHANNEL_BURST;
static uint32_t tx_last_refill_ms = 0;

void public_channel_reset_tx(void) {
    tx_tokens = BRAMBLE_PUBLIC_CHANNEL_BURST;
    tx_last_refill_ms = 0;
}

bool public_channel_can_send(uint32_t now_ms) {
    /* Refill tokens */
    if (tx_last_refill_ms == 0) {
        tx_last_refill_ms = now_ms;
    }
    uint32_t elapsed = now_ms - tx_last_refill_ms;
    uint32_t new_tokens = elapsed / BRAMBLE_PUBLIC_CHANNEL_RATE_LIMIT_MS;
    if (new_tokens > 0) {
        tx_tokens += new_tokens;
        if (tx_tokens > BRAMBLE_PUBLIC_CHANNEL_BURST)
            tx_tokens = BRAMBLE_PUBLIC_CHANNEL_BURST;
        tx_last_refill_ms += new_tokens * BRAMBLE_PUBLIC_CHANNEL_RATE_LIMIT_MS;
    }
    if (tx_tokens > 0) {
        tx_tokens--;
        return true;
    }
    return false;
}

/* RX per-source rate limiting */
#define RX_TABLE_SIZE 16
#define RX_MIN_INTERVAL_MS 10000

typedef struct {
    uint32_t src_addr;
    uint32_t last_ms;
} rx_entry_t;

static rx_entry_t rx_table[RX_TABLE_SIZE];
static int rx_count = 0;

void public_channel_reset_rx(void) {
    memset(rx_table, 0, sizeof(rx_table));
    rx_count = 0;
}

bool public_channel_rx_check(uint32_t src_addr, uint32_t now_ms) {
    /* Find existing entry */
    for (int i = 0; i < rx_count; i++) {
        if (rx_table[i].src_addr == src_addr) {
            if (now_ms - rx_table[i].last_ms < RX_MIN_INTERVAL_MS) {
                return false; /* rate limited */
            }
            rx_table[i].last_ms = now_ms;
            return true;
        }
    }
    /* New source — add entry */
    if (rx_count < RX_TABLE_SIZE) {
        rx_table[rx_count].src_addr = src_addr;
        rx_table[rx_count].last_ms = now_ms;
        rx_count++;
    } else {
        /* Evict oldest */
        int oldest = 0;
        for (int i = 1; i < RX_TABLE_SIZE; i++) {
            if (rx_table[i].last_ms < rx_table[oldest].last_ms)
                oldest = i;
        }
        rx_table[oldest].src_addr = src_addr;
        rx_table[oldest].last_ms = now_ms;
    }
    return true;
}
