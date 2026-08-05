#include "traffic_debug.h"
#include "esp_timer.h"
#include <string.h>

/* Pull the wire packet types and airtime tiers from their defining headers so
 * the classifier stays in lockstep with the protocol. Hand-copied mirrors of
 * these constants used to live here and drifted (they never grew
 * PKT_TYPE_IDENTITY_ATTESTATION), silently bucketing new packet types as
 * TRAFFIC_CAT_OTHER. */
#include "airtime_budget.h"
#include "packet.h"

void traffic_debug_init(traffic_debug_t* td, traffic_event_t* buffer, uint16_t capacity) {
    memset(td, 0, sizeof(*td));
    td->events = buffer;
    td->capacity = capacity;
    td->enabled = false;
    td->notify_cb = NULL;
    td->notify_ctx = NULL;
}

void traffic_debug_enable(traffic_debug_t* td, bool enabled) { td->enabled = enabled; }

bool traffic_debug_is_enabled(traffic_debug_t* td) { return td->enabled; }

traffic_category_t traffic_debug_classify_packet(uint8_t pkt_type) {
    switch (pkt_type) {
    case PKT_TYPE_BEACON:
        return TRAFFIC_CAT_BEACON;

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
    case PKT_TYPE_STORE_REQUEST:
    case PKT_TYPE_MAILBOX_DELIVERY:
    case PKT_TYPE_MAILBOX_QUERY:
    case PKT_TYPE_LOCATION:
    case PKT_TYPE_IDENTITY_ATTESTATION:
        return TRAFFIC_CAT_MAINTENANCE;

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

const char* traffic_debug_category_name(traffic_category_t category) {
    static const char* const names[] = {"beacon", "timesync",    "routing", "ack",
                                        "chat",   "maintenance", "other"};
    _Static_assert(sizeof(names) / sizeof(names[0]) == TRAFFIC_CAT_OTHER + 1,
                   "category name table must cover every traffic_category_t value");
    if ((unsigned)category <= (unsigned)TRAFFIC_CAT_OTHER) {
        return names[category];
    }
    return "unknown";
}

const char* traffic_debug_airtime_tier_name(uint8_t tier) {
    static const char* const names[] = {"none", "normal", "critical", "broadcast"};
    if (tier < sizeof(names) / sizeof(names[0])) {
        return names[tier];
    }
    return "unknown";
}

static void record_event(traffic_debug_t* td, uint8_t pkt_type, uint16_t len, int8_t rssi,
                         bool is_tx, uint8_t tier) {
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
    traffic_event_t* evt = &td->events[write_idx];
    evt->seq = seq;
    evt->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
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

void traffic_debug_record_tx(traffic_debug_t* td, uint8_t pkt_type, uint16_t len, uint8_t tier) {
    record_event(td, pkt_type, len, 0, true, tier);
}

void traffic_debug_record_rx(traffic_debug_t* td, uint8_t pkt_type, uint16_t len, int8_t rssi) {
    record_event(td, pkt_type, len, rssi, false, AIRTIME_TIER_NORMAL);
}

uint16_t traffic_debug_get_count(traffic_debug_t* td) { return td->count; }

uint32_t traffic_debug_get_dropped(traffic_debug_t* td) { return td->dropped_count; }

const traffic_event_t* traffic_debug_get_event(traffic_debug_t* td, uint16_t index) {
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

void traffic_debug_set_notify_callback(traffic_debug_t* td, traffic_event_cb_t cb, void* ctx) {
    td->notify_cb = cb;
    td->notify_ctx = ctx;
}
