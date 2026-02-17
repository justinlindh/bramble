#ifndef SIM_EVENT_H
#define SIM_EVENT_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_EVENT_QUEUE 100000

typedef enum {
    EVT_SEND_PACKET = 0,
    EVT_RECEIVE_PACKET,
    EVT_TIMER_FIRE,
    EVT_NODE_JOIN,
    EVT_NODE_LEAVE,
    EVT_NODE_MOVE,
    EVT_INTERFERENCE_START,
    EVT_INTERFERENCE_END,
    EVT_GENERATE_MESSAGE,
    EVT_METRICS_TICK,
    EVT_TICK_NODE,          /* per-node periodic tick */
} event_type_t;

/* Packet event data */
typedef struct {
    uint32_t src_addr;
    uint32_t dest_addr;
    int8_t   rssi;          /* RSSI at receiver */
    uint8_t  data[256];
    uint16_t len;
} packet_event_data_t;

/* Node event data */
typedef struct {
    char     node_id[16];
    uint32_t addr;
    float    x;
    float    y;
} node_event_data_t;

/* Interference event data */
typedef struct {
    int zone_index;
    float center_x;
    float center_y;
    float radius;
} interference_event_data_t;

/* Per-node tick event data */
typedef struct {
    char     node_id[16];
    uint32_t tick_seq;      /* monotonically increasing per-node */
} tick_event_data_t;

/* Event structure */
typedef struct {
    uint64_t timestamp_us;
    event_type_t type;
    union {
        packet_event_data_t   packet;
        node_event_data_t     node;
        interference_event_data_t interference;
        tick_event_data_t     tick;
        uint32_t              timer_id;
    } data;
} sim_event_t;

/* Event queue (min-heap by timestamp) */
typedef struct {
    sim_event_t events[MAX_EVENT_QUEUE];
    int count;
} event_queue_t;

void event_queue_init(event_queue_t *queue);
bool event_queue_push(event_queue_t *queue, const sim_event_t *event);
bool event_queue_pop(event_queue_t *queue, sim_event_t *out);
sim_event_t *event_queue_peek(event_queue_t *queue);
int event_queue_count(const event_queue_t *queue);

#endif /* SIM_EVENT_H */
