#ifndef DELIVERY_EVENT_RING_H
#define DELIVERY_EVENT_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DELIVERY_EVENT_RING_CAPACITY
#define DELIVERY_EVENT_RING_CAPACITY 512u
#endif

#ifndef DELIVERY_EVENT_ROUTE_MAX_HOPS
#define DELIVERY_EVENT_ROUTE_MAX_HOPS 8u
#endif

typedef struct {
    uint32_t event_seq;
    uint32_t message_id;
    uint32_t timestamp_s;
    uint32_t recipient_addr; /* 0 when not recipient-specific */
    uint32_t source_addr;
    uint8_t event_type;
    uint8_t tier;
    uint8_t route_len;
    uint8_t reserved0;
    uint32_t route_hops[DELIVERY_EVENT_ROUTE_MAX_HOPS];
} delivery_event_record_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t capacity;
    uint32_t count;
    uint32_t write_index;
    uint32_t next_seq;
} delivery_event_ring_header_t;

typedef struct {
    delivery_event_ring_header_t header;
    delivery_event_record_t records[DELIVERY_EVENT_RING_CAPACITY];
} delivery_event_ring_t;

void delivery_event_ring_init(delivery_event_ring_t* ring);

/*
 * Appends an event and assigns a monotonically increasing event_seq.
 * Returns the assigned sequence number (0 on invalid input).
 */
uint32_t delivery_event_ring_append(delivery_event_ring_t* ring,
                                    const delivery_event_record_t* event);

uint32_t delivery_event_ring_count(const delivery_event_ring_t* ring);
uint32_t delivery_event_ring_latest_seq(const delivery_event_ring_t* ring);

/*
 * Lists events in chronological order with event_seq > since_event_seq.
 * Returns number of records written to out.
 */
size_t delivery_event_ring_list_since(const delivery_event_ring_t* ring, uint32_t since_event_seq,
                                      delivery_event_record_t* out, size_t out_max);

#ifdef __cplusplus
}
#endif

#endif /* DELIVERY_EVENT_RING_H */
