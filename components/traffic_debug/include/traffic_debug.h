#ifndef BRAMBLE_TRAFFIC_DEBUG_H
#define BRAMBLE_TRAFFIC_DEBUG_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Traffic event categories for classification
 */
typedef enum {
    TRAFFIC_CAT_BEACON = 0,
    TRAFFIC_CAT_TIMESYNC,
    TRAFFIC_CAT_ROUTING,
    TRAFFIC_CAT_ACK,
    TRAFFIC_CAT_CHAT,
    TRAFFIC_CAT_MAINTENANCE,
    TRAFFIC_CAT_OTHER
} traffic_category_t;

/**
 * Traffic event structure
 * Captures essential metadata for TX/RX telemetry
 */
typedef struct {
    uint32_t seq;              /* Monotonic sequence number */
    uint32_t timestamp_ms;     /* Event timestamp */
    uint8_t pkt_type;          /* Packet type from packet.h */
    traffic_category_t category; /* Classified category */
    uint8_t airtime_tier;      /* Airtime tier (broadcast/normal/critical) */
    uint16_t packet_len;       /* Packet length in bytes */
    int8_t rssi;               /* RSSI for RX, 0 for TX */
    bool is_tx;                /* true=TX, false=RX */
} traffic_event_t;

/**
 * Traffic event notification callback
 * Called when a new event is recorded (if callback is registered)
 */
typedef void (*traffic_event_cb_t)(const traffic_event_t *evt, void *ctx);

/**
 * Traffic debug ring buffer state
 */
typedef struct {
    traffic_event_t *events;   /* Ring buffer storage */
    uint16_t capacity;         /* Buffer capacity */
    uint16_t head;             /* Write position */
    uint16_t count;            /* Number of valid events (up to capacity) */
    uint32_t dropped_count;    /* Total events dropped due to wrap */
    uint32_t next_seq;         /* Next sequence number to assign */
    bool enabled;              /* Runtime enable/disable gate */
    traffic_event_cb_t notify_cb; /* Notification callback */
    void *notify_ctx;          /* Callback context */
} traffic_debug_t;

/**
 * Initialize traffic debug ring buffer
 * @param td Traffic debug instance
 * @param buffer Event storage array
 * @param capacity Buffer capacity
 */
void traffic_debug_init(traffic_debug_t *td, traffic_event_t *buffer, uint16_t capacity);

/**
 * Enable or disable traffic debug event recording
 * @param td Traffic debug instance
 * @param enabled true to enable, false to disable
 */
void traffic_debug_enable(traffic_debug_t *td, bool enabled);

/**
 * Check if traffic debug is enabled
 * @param td Traffic debug instance
 * @return true if enabled
 */
bool traffic_debug_is_enabled(traffic_debug_t *td);

/**
 * Classify packet type into traffic category
 * @param pkt_type Packet type from packet.h
 * @return Traffic category
 */
traffic_category_t traffic_debug_classify_packet(uint8_t pkt_type);

/**
 * Map category + tier hint to final airtime tier
 * Some categories override the tier hint (beacon/timesync always broadcast)
 * @param category Traffic category
 * @param tier_hint Original airtime tier
 * @return Final airtime tier to use
 */
uint8_t traffic_debug_get_airtime_tier(traffic_category_t category, uint8_t tier_hint);

/**
 * Record TX event
 * @param td Traffic debug instance
 * @param pkt_type Packet type
 * @param len Packet length
 * @param tier Airtime tier
 */
void traffic_debug_record_tx(traffic_debug_t *td, uint8_t pkt_type, uint16_t len, uint8_t tier);

/**
 * Record RX event
 * @param td Traffic debug instance
 * @param pkt_type Packet type
 * @param len Packet length
 * @param rssi RSSI value
 */
void traffic_debug_record_rx(traffic_debug_t *td, uint8_t pkt_type, uint16_t len, int8_t rssi);

/**
 * Get number of events currently in buffer
 * @param td Traffic debug instance
 * @return Event count
 */
uint16_t traffic_debug_get_count(traffic_debug_t *td);

/**
 * Get total number of dropped events
 * @param td Traffic debug instance
 * @return Dropped event count
 */
uint32_t traffic_debug_get_dropped(traffic_debug_t *td);

/**
 * Get event by index (0 = oldest visible event)
 * @param td Traffic debug instance
 * @param index Event index
 * @return Pointer to event, or NULL if invalid
 */
const traffic_event_t *traffic_debug_get_event(traffic_debug_t *td, uint16_t index);

/**
 * Register notification callback for real-time event stream
 * @param td Traffic debug instance
 * @param cb Callback function (NULL to unregister)
 * @param ctx Context pointer passed to callback
 */
void traffic_debug_set_notify_callback(traffic_debug_t *td, traffic_event_cb_t cb, void *ctx);

#endif /* BRAMBLE_TRAFFIC_DEBUG_H */
