#ifndef BRAMBLE_DUMMY_TRAFFIC_H
#define BRAMBLE_DUMMY_TRAFFIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Dummy traffic configuration
#define DUMMY_TRAFFIC_MIN_INTERVAL_MS 5000  // 5 seconds minimum
#define DUMMY_TRAFFIC_MAX_INTERVAL_MS 30000 // 30 seconds maximum
#define DUMMY_TRAFFIC_MIN_SIZE 20           // Minimum dummy packet size
#define DUMMY_TRAFFIC_MAX_SIZE 120          // Maximum dummy packet size
#define DUMMY_TRAFFIC_AIRTIME_BUDGET_PCT 2  // Max 2% of airtime budget for dummies

typedef struct {
    bool enabled;              // Privacy mode on/off
    uint32_t next_send_time;   // When to send next dummy
    uint32_t total_dummy_sent; // Counter for stats
    uint32_t airtime_used_ms;  // Airtime consumed by dummies in current window
    uint32_t window_start;     // Start of current tracking window
} dummy_traffic_ctx_t;

void dummy_traffic_init(dummy_traffic_ctx_t* ctx);
void dummy_traffic_enable(dummy_traffic_ctx_t* ctx, bool enabled, uint32_t now_ms);
bool dummy_traffic_is_enabled(const dummy_traffic_ctx_t* ctx);

// Called periodically. Returns true if a dummy packet should be sent now.
// If true, fills size_out with random packet size to generate.
bool dummy_traffic_should_send(dummy_traffic_ctx_t* ctx, uint32_t now_ms,
                               uint32_t airtime_budget_remaining_ms, size_t* size_out);

// Called after sending a dummy. Records airtime used.
void dummy_traffic_record_send(dummy_traffic_ctx_t* ctx, uint32_t airtime_ms, uint32_t now_ms);

// Build a dummy packet that looks like a real encrypted DATA packet
int dummy_traffic_build_packet(uint8_t* out, size_t size, uint32_t my_addr);

// Get stats
uint32_t dummy_traffic_get_count(const dummy_traffic_ctx_t* ctx);

#endif
