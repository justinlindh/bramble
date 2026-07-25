#include "delivery_event_ring.h"

#include <string.h>

#define DELIVERY_EVENT_RING_MAGIC 0x44565247u /* "DVRG" */
#define DELIVERY_EVENT_RING_VERSION 1u

static uint32_t ring_oldest_index(const delivery_event_ring_t* ring) {
    if (!ring) {
        return 0u;
    }
    if (ring->header.count < ring->header.capacity) {
        return 0u;
    }
    return ring->header.write_index;
}

void delivery_event_ring_init(delivery_event_ring_t* ring) {
    if (!ring) {
        return;
    }
    memset(ring, 0, sizeof(*ring));
    ring->header.magic = DELIVERY_EVENT_RING_MAGIC;
    ring->header.version = DELIVERY_EVENT_RING_VERSION;
    ring->header.capacity = DELIVERY_EVENT_RING_CAPACITY;
    ring->header.count = 0u;
    ring->header.write_index = 0u;
    ring->header.next_seq = 1u;
}

uint32_t delivery_event_ring_append(delivery_event_ring_t* ring,
                                    const delivery_event_record_t* event) {
    delivery_event_record_t stored;

    if (!ring || !event || ring->header.capacity == 0u) {
        return 0u;
    }
    if (ring->header.magic != DELIVERY_EVENT_RING_MAGIC ||
        ring->header.version != DELIVERY_EVENT_RING_VERSION) {
        return 0u;
    }

    stored = *event;
    if (stored.route_len > DELIVERY_EVENT_ROUTE_MAX_HOPS) {
        stored.route_len = DELIVERY_EVENT_ROUTE_MAX_HOPS;
    }

    stored.event_seq = ring->header.next_seq;
    ring->records[ring->header.write_index] = stored;

    ring->header.write_index = (ring->header.write_index + 1u) % ring->header.capacity;
    if (ring->header.count < ring->header.capacity) {
        ring->header.count++;
    }
    ring->header.next_seq++;

    return stored.event_seq;
}

uint32_t delivery_event_ring_count(const delivery_event_ring_t* ring) {
    return ring ? ring->header.count : 0u;
}

uint32_t delivery_event_ring_latest_seq(const delivery_event_ring_t* ring) {
    if (!ring || ring->header.count == 0u || ring->header.next_seq == 0u) {
        return 0u;
    }
    return ring->header.next_seq - 1u;
}

size_t delivery_event_ring_list_since(const delivery_event_ring_t* ring, uint32_t since_event_seq,
                                      delivery_event_record_t* out, size_t out_max) {
    size_t written = 0u;
    uint32_t i;
    uint32_t oldest;

    if (!ring || !out || out_max == 0u || ring->header.count == 0u) {
        return 0u;
    }

    oldest = ring_oldest_index(ring);
    for (i = 0u; i < ring->header.count && written < out_max; i++) {
        uint32_t idx = (oldest + i) % ring->header.capacity;
        const delivery_event_record_t* rec = &ring->records[idx];
        if (rec->event_seq > since_event_seq) {
            out[written++] = *rec;
        }
    }

    return written;
}
