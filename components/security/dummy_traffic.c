#include "dummy_traffic.h"
#include "packet.h"
#include <string.h>
#include <stdlib.h>

// Simple deterministic random for non-crypto use (test-friendly via srand)
static uint32_t dummy_rand_range(uint32_t min, uint32_t max) {
    if (min >= max) return min;
    return min + ((uint32_t)rand() % (max - min + 1));
}

static uint32_t schedule_next(uint32_t now_ms) {
    uint32_t interval = dummy_rand_range(DUMMY_TRAFFIC_MIN_INTERVAL_MS,
                                          DUMMY_TRAFFIC_MAX_INTERVAL_MS);
    return now_ms + interval;
}

void dummy_traffic_init(dummy_traffic_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

void dummy_traffic_enable(dummy_traffic_ctx_t *ctx, bool enabled, uint32_t now_ms) {
    ctx->enabled = enabled;
    if (enabled) {
        ctx->next_send_time = schedule_next(now_ms);
        ctx->window_start = now_ms;
        ctx->airtime_used_ms = 0;
    }
}

bool dummy_traffic_is_enabled(const dummy_traffic_ctx_t *ctx) {
    return ctx->enabled;
}

bool dummy_traffic_should_send(dummy_traffic_ctx_t *ctx, uint32_t now_ms,
                                uint32_t airtime_budget_remaining_ms,
                                size_t *size_out) {
    if (!ctx->enabled) return false;
    if (now_ms < ctx->next_send_time) return false;

    // Check airtime budget: dummies shouldn't exceed 2% of total budget
    // airtime_budget_remaining_ms is how much total budget remains
    // We check if our dummy airtime is already >= 2% of total (remaining + used)
    uint32_t total_budget = airtime_budget_remaining_ms + ctx->airtime_used_ms;
    uint32_t max_dummy_airtime = (total_budget * DUMMY_TRAFFIC_AIRTIME_BUDGET_PCT) / 100;
    if (ctx->airtime_used_ms >= max_dummy_airtime) {
        // Still reschedule so we check again later
        ctx->next_send_time = schedule_next(now_ms);
        return false;
    }

    *size_out = dummy_rand_range(DUMMY_TRAFFIC_MIN_SIZE, DUMMY_TRAFFIC_MAX_SIZE);
    ctx->next_send_time = schedule_next(now_ms);
    return true;
}

void dummy_traffic_record_send(dummy_traffic_ctx_t *ctx, uint32_t airtime_ms, uint32_t now_ms) {
    ctx->airtime_used_ms += airtime_ms;
    ctx->total_dummy_sent++;

    // Reset window every 60 seconds
    if ((now_ms - ctx->window_start) >= 60000) {
        ctx->window_start = now_ms;
        ctx->airtime_used_ms = 0;
    }
}

int dummy_traffic_build_packet(uint8_t *out, size_t size, uint32_t my_addr) {
    if (size < HEADER_SIZE) return -1;

    // Fill entire packet with random bytes first (looks like ciphertext)
    for (size_t i = 0; i < size; i++) {
        out[i] = (uint8_t)(rand() & 0xFF);
    }

    // Build a proper DATA header
    bramble_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.version = BRAMBLE_VERSION;
    hdr.type = PKT_TYPE_DATA;
    hdr.flags = FLAG_ENCRYPT;  // Looks like encrypted data
    hdr.hop_limit = 1;         // Don't relay — local cover traffic only

    // Random dest_addr, avoiding broadcast (0xFFFFFFFF) and null (0x00000000)
    do {
        hdr.dest_addr = ((uint32_t)rand() << 16) | (uint32_t)rand();
    } while (hdr.dest_addr == 0xFFFFFFFF || hdr.dest_addr == 0x00000000);

    // Random packet_id
    hdr.packet_id = ((uint32_t)rand() << 16) | (uint32_t)rand();

    // Serialize header into the buffer
    bramble_header_serialize(&hdr, out, size);

    // After header, write src_addr (like real DATA packets)
    if (size >= HEADER_SIZE + 4) {
        memcpy(out + HEADER_SIZE, &my_addr, 4);
    }

    return 0;
}

uint32_t dummy_traffic_get_count(const dummy_traffic_ctx_t *ctx) {
    return ctx->total_dummy_sent;
}
