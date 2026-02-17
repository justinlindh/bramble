#ifndef BRAMBLE_PROBE_H
#define BRAMBLE_PROBE_H

#include <stdint.h>
#include <stdbool.h>

/* Packet types */
#define BRAMBLE_TYPE_BROADCAST_PROBE  0x12
#define BRAMBLE_TYPE_BROADCAST_ACK    0x13

/* Probe flags */
#define PROBE_FLAG_INCLUDE_RSSI  0x01
#define PROBE_FLAG_INCLUDE_PATH  0x02
#define PROBE_FLAG_SILENT        0x04

/* Beacon flag for probe ACK capability */
#define BEACON_FLAG_PROBE_ACK    0x02

/* Rate limiting defaults */
#define PROBE_RATE_LIMIT_TOKENS     3
#define PROBE_RATE_LIMIT_REFILL_MS  60000   /* 1 token per minute */
#define PROBE_ACK_COOLDOWN_MS       10000
#define PROBE_COLLECTION_WINDOW_MS  30000
#define PROBE_ACK_JITTER_MIN_MS     100
#define PROBE_ACK_JITTER_MAX_MS     2000
#define PROBE_DEDUP_SIZE            16
#define PROBE_MAX_RESPONSES         32

/* Probe packet: 12 bytes on wire */
typedef struct __attribute__((packed)) {
    uint8_t  version;       /* BRAMBLE_VERSION */
    uint8_t  type;          /* BRAMBLE_TYPE_BROADCAST_PROBE */
    uint8_t  flags;         /* PROBE_FLAG_* */
    uint8_t  hop_limit;
    uint32_t src_addr;
    uint32_t probe_id;
} bramble_probe_packet_t;

/* Probe ACK: variable length on wire */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;          /* BRAMBLE_TYPE_BROADCAST_ACK */
    uint8_t  flags;
    uint8_t  hop_count;
    uint32_t src_addr;      /* ACK sender */
    uint32_t probe_id;
    int8_t   rssi;          /* optional, present if PROBE_FLAG_INCLUDE_RSSI */
    /* path hops follow if PROBE_FLAG_INCLUDE_PATH */
} bramble_probe_ack_t;

/* Single response from a node */
typedef struct {
    uint32_t responder_addr;
    uint8_t  hop_count;
    int8_t   rssi;
    bool     has_rssi;
    uint32_t latency_ms;
} probe_response_t;

/* Collected result for a probe */
typedef struct {
    uint32_t probe_id;
    uint32_t start_ms;
    uint16_t response_count;
    bool     complete;
    probe_response_t responses[PROBE_MAX_RESPONSES];
} probe_result_t;

/* Rate limiter state */
typedef struct {
    uint8_t  tokens;
    uint8_t  max_tokens;
    uint32_t last_refill_ms;
    uint32_t refill_interval_ms;
} probe_rate_limit_t;

/* Pending ACK for delayed sending */
typedef struct {
    bool     active;
    uint32_t send_at_ms;
    bramble_probe_ack_t ack;
    uint16_t ack_len;
} pending_probe_ack_t;

/* Callback to send a packet */
typedef void (*probe_send_fn)(const uint8_t *data, uint16_t len, void *ctx);

/* Main probe state */
typedef struct {
    uint32_t my_addr;
    probe_rate_limit_t send_limiter;
    uint32_t last_ack_sent_ms;

    /* Dedup ring buffer */
    uint32_t seen_probes[PROBE_DEDUP_SIZE];
    uint8_t  seen_index;

    /* Active collection */
    probe_result_t result;
    bool collecting;

    /* Pending delayed ACK */
    pending_probe_ack_t pending_ack;

    /* Send callback */
    probe_send_fn send_fn;
    void *send_ctx;
} bramble_probe_state_t;

void bramble_probe_init(bramble_probe_state_t *state, uint32_t my_addr,
                        probe_send_fn send_fn, void *send_ctx);
int  bramble_probe_send(bramble_probe_state_t *state, uint8_t flags, uint32_t now_ms);
void bramble_probe_handle_probe(bramble_probe_state_t *state,
                                const uint8_t *data, uint16_t len,
                                int8_t rssi, uint32_t now_ms);
void bramble_probe_handle_ack(bramble_probe_state_t *state,
                              const uint8_t *data, uint16_t len,
                              uint32_t now_ms);
const probe_result_t *bramble_probe_get_result(const bramble_probe_state_t *state);
bool bramble_probe_can_send(const bramble_probe_state_t *state, uint32_t now_ms);
uint32_t bramble_probe_get_rate_limit_remaining_sec(const bramble_probe_state_t *state, uint32_t now_ms);
void bramble_probe_tick(bramble_probe_state_t *state, uint32_t now_ms);

#endif /* BRAMBLE_PROBE_H */
