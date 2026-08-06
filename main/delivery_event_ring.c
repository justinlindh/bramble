#include "delivery_event_ring.h"

#include <stdbool.h>
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

#define RECEIPT_DEDUPE_MAX 64u

size_t delivery_event_ring_receipts_for_message(const delivery_event_ring_t* ring,
                                                uint32_t message_id, uint32_t* out,
                                                size_t out_max, size_t* total_unique) {
    if (total_unique)
        *total_unique = 0;
    if (!ring || message_id == 0)
        return 0;

    uint32_t seen[RECEIPT_DEDUPE_MAX];
    size_t seen_count = 0;
    size_t written = 0;

    uint32_t count = ring->header.count;
    if (count > ring->header.capacity)
        count = ring->header.capacity;

    /* Chronological order like list_since: oldest first, so the first
     * receipt to arrive is the first name shown. */
    uint32_t start = (ring->header.count > ring->header.capacity)
                         ? ring->header.write_index
                         : 0u;
    for (uint32_t i = 0; i < count; i++) {
        const delivery_event_record_t* e = &ring->records[(start + i) % ring->header.capacity];
        if (e->message_id != message_id || e->recipient_addr == 0)
            continue;
        bool dup = false;
        for (size_t j = 0; j < seen_count; j++) {
            if (seen[j] == e->recipient_addr) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        if (seen_count < RECEIPT_DEDUPE_MAX)
            seen[seen_count++] = e->recipient_addr;
        if (out && written < out_max)
            out[written++] = e->recipient_addr;
    }

    if (total_unique)
        *total_unique = seen_count;
    return written;
}
