#include "traffic_debug.h"
#include <string.h>

/* Packet type constants from packet.h */
#define PKT_TYPE_ACK              0x01
#define PKT_TYPE_RREQ             0x02
#define PKT_TYPE_RREP             0x03
#define PKT_TYPE_RERR             0x04
#define PKT_TYPE_BEACON           0x05
#define PKT_TYPE_KEY_EXCHANGE     0x06
#define PKT_TYPE_DELIVERY_RECEIPT 0x07
#define PKT_TYPE_CONGESTION       0x08
#define PKT_TYPE_TIME_SYNC        0x09
#define PKT_TYPE_DATA             0x0A
#define PKT_TYPE_STORE_REQUEST    0x0B
#define PKT_TYPE_STORE_ACK        0x0C
#define PKT_TYPE_MAILBOX_DELIVERY 0x0D
#define PKT_TYPE_MAILBOX_QUERY    0x0E
#define PKT_TYPE_EMERGENCY        0x0F
#define PKT_TYPE_EMERGENCY_CANCEL 0x10
#define PKT_TYPE_CODED            0x11
#define PKT_TYPE_PROBE            0x12
#define PKT_TYPE_PROBE_ACK        0x13
#define PKT_TYPE_LOCATION         0x14

/* Airtime tier constants from airtime_budget.h */
#define AIRTIME_TIER_NORMAL    0x01
#define AIRTIME_TIER_CRITICAL  0x02
#define AIRTIME_TIER_BROADCAST 0x03

void traffic_debug_init(traffic_debug_t *td, traffic_event_t *buffer, uint16_t capacity) {
    memset(td, 0, sizeof(*td));
    td->events = buffer;
    td->capacity = capacity;
    td->enabled = false;
    td->notify_cb = NULL;
    td->notify_ctx = NULL;
}

void traffic_debug_enable(traffic_debug_t *td, bool enabled) {
    td->enabled = enabled;
}

bool traffic_debug_is_enabled(traffic_debug_t *td) {
    return td->enabled;
}

traffic_category_t traffic_debug_classify_packet(uint8_t pkt_type) {
    switch (pkt_type) {
        case PKT_TYPE_BEACON:
            return TRAFFIC_CAT_BEACON;
        
        case PKT_TYPE_TIME_SYNC:
            return TRAFFIC_CAT_TIMESYNC;
        
        case PKT_TYPE_RREQ:
        case PKT_TYPE_RREP:
        case PKT_TYPE_RERR:
        case PKT_TYPE_PROBE:
        case PKT_TYPE_PROBE_ACK:
            return TRAFFIC_CAT_ROUTING;
        
        case PKT_TYPE_ACK:
        case PKT_TYPE_DELIVERY_RECEIPT:
        case PKT_TYPE_STORE_ACK:
            return TRAFFIC_CAT_ACK;
        
        case PKT_TYPE_DATA:
            return TRAFFIC_CAT_CHAT;
        
        case PKT_TYPE_KEY_EXCHANGE:
        case PKT_TYPE_CONGESTION:
        case PKT_TYPE_STORE_REQUEST:
        case PKT_TYPE_MAILBOX_DELIVERY:
        case PKT_TYPE_MAILBOX_QUERY:
        case PKT_TYPE_CODED:
        case PKT_TYPE_LOCATION:
            return TRAFFIC_CAT_MAINTENANCE;
        
        case PKT_TYPE_EMERGENCY:
        case PKT_TYPE_EMERGENCY_CANCEL:
        default:
            return TRAFFIC_CAT_OTHER;
    }
}

uint8_t traffic_debug_get_airtime_tier(traffic_category_t category, uint8_t tier_hint) {
    switch (category) {
        case TRAFFIC_CAT_BEACON:
        case TRAFFIC_CAT_TIMESYNC:
            /* Beacon and timesync always use broadcast tier */
            return AIRTIME_TIER_BROADCAST;
        
        case TRAFFIC_CAT_ROUTING:
        case TRAFFIC_CAT_ACK:
        case TRAFFIC_CAT_MAINTENANCE:
            /* These always use normal tier */
            return AIRTIME_TIER_NORMAL;
        
        case TRAFFIC_CAT_CHAT:
        case TRAFFIC_CAT_OTHER:
            /* Chat and other respect the tier hint */
            return tier_hint;
        
        default:
            return tier_hint;
    }
}

static void record_event(traffic_debug_t *td, uint8_t pkt_type, uint16_t len, 
                        int8_t rssi, bool is_tx, uint8_t tier) {
    if (!td->enabled) {
        return;
    }
    
    /* Classify packet */
    traffic_category_t category = traffic_debug_classify_packet(pkt_type);
    uint8_t final_tier = traffic_debug_get_airtime_tier(category, tier);
    
    /* Allocate sequence number */
    uint32_t seq = td->next_seq++;
    
    /* Calculate write position */
    uint16_t write_idx = td->head;
    
    /* Check if we're dropping an old event */
    if (td->count == td->capacity) {
        td->dropped_count++;
    } else {
        td->count++;
    }
    
    /* Write event */
    traffic_event_t *evt = &td->events[write_idx];
    evt->seq = seq;
    evt->timestamp_ms = 0;  /* TODO: Add timestamp when ESP-IDF timer available */
    evt->pkt_type = pkt_type;
    evt->category = category;
    evt->airtime_tier = final_tier;
    evt->packet_len = len;
    evt->rssi = rssi;
    evt->is_tx = is_tx;
    
    /* Advance head (circular) */
    td->head = (td->head + 1) % td->capacity;
    
    /* Notify callback if registered */
    if (td->notify_cb) {
        td->notify_cb(evt, td->notify_ctx);
    }
}

void traffic_debug_record_tx(traffic_debug_t *td, uint8_t pkt_type, uint16_t len, uint8_t tier) {
    record_event(td, pkt_type, len, 0, true, tier);
}

void traffic_debug_record_rx(traffic_debug_t *td, uint8_t pkt_type, uint16_t len, int8_t rssi) {
    record_event(td, pkt_type, len, rssi, false, AIRTIME_TIER_NORMAL);
}

uint16_t traffic_debug_get_count(traffic_debug_t *td) {
    return td->count;
}

uint32_t traffic_debug_get_dropped(traffic_debug_t *td) {
    return td->dropped_count;
}

const traffic_event_t *traffic_debug_get_event(traffic_debug_t *td, uint16_t index) {
    if (index >= td->count) {
        return NULL;
    }
    
    /* When buffer is full, oldest event is at head position
     * When buffer is not full, oldest event is at index 0 */
    uint16_t physical_idx;
    if (td->count < td->capacity) {
        physical_idx = index;
    } else {
        physical_idx = (td->head + index) % td->capacity;
    }
    
    return &td->events[physical_idx];
}

void traffic_debug_set_notify_callback(traffic_debug_t *td, traffic_event_cb_t cb, void *ctx) {
    td->notify_cb = cb;
    td->notify_ctx = ctx;
}
