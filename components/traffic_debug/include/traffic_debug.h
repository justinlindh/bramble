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

/* Depth of the traffic event ring, overridable per platform at compile time,
 * the same way DELIVERY_EVENT_RING_CAPACITY is. The ring is one of the largest
 * static allocations on the nRF52840, where RAM is the binding constraint and
 * the budget gate leaves under a kilobyte of headroom, so that target defines a
 * smaller value in its own CMakeLists rather than this header carrying a
 * platform ifdef. It holds debug telemetry, not anything the mesh needs to
 * function, so a shorter history is the right thing to give up there. */
#ifndef TRAFFIC_DEBUG_CAPACITY
#define TRAFFIC_DEBUG_CAPACITY 512
#endif

/**
 * Traffic event structure
 * Captures essential metadata for TX/RX telemetry
 *
 * Field order is deliberate: the four-byte members lead, then the small ones
 * pack into the tail. This struct is the element type of a ring buffer sized
 * in the hundreds, so a member landing after a bool costs four bytes of
 * padding per event, and on the nRF52840 that is enough to breach the static
 * RAM budget. Reorder only with the resulting sizeof in mind; nothing here is
 * a wire layout, so the order itself carries no compatibility meaning. */
typedef struct {
    uint32_t seq;          /* Monotonic sequence number */
    uint32_t timestamp_ms; /* Event timestamp */
    /* Claimed origin of an RX frame, 0 when unknown or for TX. Without it an
     * RSSI sample cannot be tied to a peer, which makes the event stream
     * useless for per-link RF work: neighbor-table RSSI only refreshes on
     * beacons, so this is the only per-packet signal-strength record. Read off
     * the unauthenticated wire prefix (see bramble_packet_origin_addr), so it
     * is telemetry, never a trust input. */
    uint32_t src_addr;
    traffic_category_t category; /* Classified category */
    uint8_t pkt_type;            /* Packet type from packet.h */
    uint8_t airtime_tier;        /* Airtime tier (broadcast/normal/critical) */
    uint16_t packet_len;         /* Packet length in bytes */
    int8_t rssi;                 /* RSSI for RX, 0 for TX */
    bool is_tx;                  /* true=TX, false=RX */
} traffic_event_t;

/**
 * Traffic event notification callback
 * Called when a new event is recorded (if callback is registered)
 */
typedef void (*traffic_event_cb_t)(const traffic_event_t* evt, void* ctx);

/**
 * Traffic debug ring buffer state
 */
typedef struct {
    traffic_event_t* events;      /* Ring buffer storage */
    uint16_t capacity;            /* Buffer capacity */
    uint16_t head;                /* Write position */
    uint16_t count;               /* Number of valid events (up to capacity) */
    uint32_t dropped_count;       /* Total events dropped due to wrap */
    uint32_t next_seq;            /* Next sequence number to assign */
    bool enabled;                 /* Runtime enable/disable gate */
    traffic_event_cb_t notify_cb; /* Notification callback */
    void* notify_ctx;             /* Callback context */
} traffic_debug_t;

/**
 * Initialize traffic debug ring buffer
 * @param td Traffic debug instance
 * @param buffer Event storage array
 * @param capacity Buffer capacity
 */
void traffic_debug_init(traffic_debug_t* td, traffic_event_t* buffer, uint16_t capacity);

/**
 * Enable or disable traffic debug event recording
 * @param td Traffic debug instance
 * @param enabled true to enable, false to disable
 */
void traffic_debug_enable(traffic_debug_t* td, bool enabled);

/**
 * Check if traffic debug is enabled
 * @param td Traffic debug instance
 * @return true if enabled
 */
bool traffic_debug_is_enabled(traffic_debug_t* td);

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
 * Canonical lowercase category name for JSON/telemetry serialization
 * (e.g. TRAFFIC_CAT_ROUTING -> "routing"). Its bound derives from the enum,
 * so adding a category cannot silently mis-serialize as a stale string.
 * @param category Traffic category
 * @return Static string, "unknown" for out-of-range values
 */
const char* traffic_debug_category_name(traffic_category_t category);

/**
 * Canonical airtime-tier name for JSON/telemetry serialization:
 * 0 -> "none", 1 -> "normal", 2 -> "critical", 3 -> "broadcast"
 * (the AIRTIME_TIER_* values, with 0 for an unset tier).
 * @param tier Raw airtime-tier value from a traffic_event_t
 * @return Static string, "unknown" for out-of-range values
 */
const char* traffic_debug_airtime_tier_name(uint8_t tier);

/**
 * Record TX event
 * @param td Traffic debug instance
 * @param pkt_type Packet type
 * @param len Packet length
 * @param tier Airtime tier
 */
void traffic_debug_record_tx(traffic_debug_t* td, uint8_t pkt_type, uint16_t len, uint8_t tier);

/**
 * Record RX event
 * @param td Traffic debug instance
 * @param pkt_type Packet type
 * @param len Packet length
 * @param rssi RSSI value
 * @param src_addr Claimed origin address, or 0 when the frame's type carries
 *                 none (see bramble_packet_origin_addr). Telemetry only.
 */
void traffic_debug_record_rx(traffic_debug_t* td, uint8_t pkt_type, uint16_t len, int8_t rssi,
                             uint32_t src_addr);

/**
 * Get number of events currently in buffer
 * @param td Traffic debug instance
 * @return Event count
 */
uint16_t traffic_debug_get_count(traffic_debug_t* td);

/**
 * Get total number of dropped events
 * @param td Traffic debug instance
 * @return Dropped event count
 */
uint32_t traffic_debug_get_dropped(traffic_debug_t* td);

/**
 * Get event by index (0 = oldest visible event)
 * @param td Traffic debug instance
 * @param index Event index
 * @return Pointer to event, or NULL if invalid
 */
const traffic_event_t* traffic_debug_get_event(traffic_debug_t* td, uint16_t index);

/**
 * Register notification callback for real-time event stream
 * @param td Traffic debug instance
 * @param cb Callback function (NULL to unregister)
 * @param ctx Context pointer passed to callback
 */
void traffic_debug_set_notify_callback(traffic_debug_t* td, traffic_event_cb_t cb, void* ctx);

#endif /* BRAMBLE_TRAFFIC_DEBUG_H */
