#ifndef SIM_EVENT_H
#define SIM_EVENT_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_EVENT_QUEUE 100000

typedef enum {
    /* Repurposed (Task 5, channel flood): a due-timestamped jittered
     * channel-flood relay. data.packet.src_addr carries the RELAYING
     * node's own address (which node fires this event, not a radio
     * source); data.packet.data/len is the exact relay-mutated frame to
     * transmit. See simulator/gosim/bridge.c's bridge_handle_flood_relay. */
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
    EVT_BROADCAST_DELIVERY, /* synthetic bramble.onBroadcastDelivery notification */
    /* Per-node identity Phase 3: a scripted identity-attestation
     * origination ("send_attestation" scenario event). data.node.node_id
     * is the ORIGINATING node; data.node.addr is the CLAIMED address (0 =
     * the node's own address; a nonzero different address models a keyed
     * insider impersonating another node, the conflict-detection case). */
    EVT_GENERATE_ATTESTATION,
    /* Trust-anchor campaign (P2 red-team): a scripted runtime anchor
     * provisioning ("provision_anchor" scenario event), the sim analog of an
     * operator running bramble.setAnchor mid-life to harden an un-anchored
     * fleet. data.node.node_id is the node being (re-)anchored to the fleet
     * test anchor; the drop-stale-pins behavior of identity_store_set_anchor is
     * what the event exercises. */
    EVT_PROVISION_ANCHOR,
    /* Location sharing (issue #172): a scripted GPS position broadcast
     * ("send_location" scenario event). data.location.node_id is the
     * originating node; the fix-degree coordinates from the scenario are
     * carried as e7 integers (the firmware's own bramble_position_t
     * representation) so no float precision is lost on the way to
     * location_serialize_for_tier. */
    EVT_GENERATE_LOCATION,
    /* Receipt reliability campaign: one pending broadcast delivery receipt
     * has come due for transmission on one node, the sim analog of
     * firmware's MESH_EVT_RECEIPT_TX (main/mesh_internal.h) fired by the
     * s_receipt_timer. data.receipt_tx names the node and its queue slot;
     * the receipt bytes and the attempt/defer counters live in that slot
     * (bridge.h's bridge_node_ext_t.receipt_queue), exactly like firmware
     * keeps them in s_receipt_queue rather than in the event. */
    EVT_RECEIPT_TX,
    /* Attested roll-call: a scripted initiation ("start_rollcall" scenario
     * event). data.rollcall names the initiating node and carries the
     * operator payload the announce floods. */
    EVT_GENERATE_ROLLCALL,
    /* One roll-call announce round has come due on the initiator, the sim
     * analog of the round schedule firmware drives from its maintenance
     * tick. data.rollcall_slot.round is the 1-based round to send, or 0 for
     * the close sweep scheduled after the last round. */
    EVT_ROLLCALL_ROUND,
    /* One member's staggered answer has come due. data.rollcall_slot names
     * the node and its pending-answer slot; the answer itself lives in that
     * slot (bridge.h's bridge_node_ext_t.rollcall_pending), exactly like the
     * receipt queue above keeps its bytes out of the event. */
    EVT_ROLLCALL_TX,
} event_type_t;

/* Packet event data */
typedef struct {
    uint32_t src_addr;
    uint32_t dest_addr;
    int8_t rssi; /* RSSI at receiver (dBm) */
    int8_t snr;  /* SNR at receiver (dB, typical LoRa range 0-50) */
    uint8_t data[256];
    uint16_t len;
    /* On-air occupancy window of the transmission that produced this event.
     * Used by the collision model to evaluate overlap at delivery time. */
    uint64_t air_start_us;
    uint64_t air_end_us;
    float tx_x; /* transmitter position at TX time (capture-effect RSSI math) */
    float tx_y;
} packet_event_data_t;

/* Node event data */
typedef struct {
    char node_id[16];
    uint32_t addr;
    float x;
    float y;
    /* node_join only (issue #144): true when the scenario event supplied
     * explicit x/y. A coordinate-less rejoin restores the node's original
     * scenario position instead of teleporting it to (0,0). */
    bool has_coords;
} node_event_data_t;

/* Location event data (EVT_GENERATE_LOCATION). Degrees are stored as
 * degrees * 1e7 (the firmware's bramble_position_t convention): a float
 * only holds ~7 significant digits, which would corrupt e7 coordinates. */
typedef struct {
    char node_id[16];
    int32_t latitude_e7;
    int32_t longitude_e7;
    int16_t altitude_m;
} location_event_data_t;

/* Pending-receipt transmission event data (EVT_RECEIPT_TX) */
typedef struct {
    uint32_t node_addr; /* which node's receipt queue this fires on */
    int slot;           /* index into that node's receipt_queue */
} receipt_tx_event_data_t;

/* Roll-call initiation event data (EVT_GENERATE_ROLLCALL). The operator
 * payload is carried inline because it is the whole content of the announce.
 * SIM_ROLLCALL_TEXT_MAX must equal components/rollcall's ROLLCALL_TEXT_MAX;
 * bridge.c static-asserts that rather than trusting the two to stay in step,
 * because the engine deliberately includes no component headers. */
#define SIM_ROLLCALL_TEXT_MAX 48
typedef struct {
    char node_id[16];
    char text[SIM_ROLLCALL_TEXT_MAX + 1];
} rollcall_event_data_t;

/* Roll-call round / staggered-answer event data (EVT_ROLLCALL_ROUND,
 * EVT_ROLLCALL_TX). One struct for both because both name a node and one
 * index into its roll-call state: EVT_ROLLCALL_ROUND uses `round` (0 = the
 * close sweep), EVT_ROLLCALL_TX uses `slot`. */
typedef struct {
    uint32_t node_addr;
    int slot;
    uint8_t round;
} rollcall_slot_event_data_t;

/* Interference event data */
typedef struct {
    int zone_index;
    float center_x;
    float center_y;
    float radius;
} interference_event_data_t;

/* Per-node tick event data */
typedef struct {
    char node_id[16];
    uint32_t tick_seq; /* monotonically increasing per-node */
} tick_event_data_t;

/* Synthetic broadcast delivery telemetry event data */
typedef struct {
    char packet_id[16];
    char recipient[16];
    uint8_t hop_count;
} broadcast_delivery_event_data_t;

/* Event structure */
typedef struct {
    uint64_t timestamp_us;
    event_type_t type;
    union {
        packet_event_data_t packet;
        node_event_data_t node;
        location_event_data_t location;
        interference_event_data_t interference;
        tick_event_data_t tick;
        broadcast_delivery_event_data_t broadcast_delivery;
        receipt_tx_event_data_t receipt_tx;
        rollcall_event_data_t rollcall;
        rollcall_slot_event_data_t rollcall_slot;
        uint32_t timer_id;
    } data;
} sim_event_t;

/* Event queue (min-heap by timestamp) */
typedef struct {
    sim_event_t events[MAX_EVENT_QUEUE];
    int count;
} event_queue_t;

void event_queue_init(event_queue_t* queue);
bool event_queue_push(event_queue_t* queue, const sim_event_t* event);
bool event_queue_pop(event_queue_t* queue, sim_event_t* out);
sim_event_t* event_queue_peek(event_queue_t* queue);
int event_queue_count(const event_queue_t* queue);

#endif /* SIM_EVENT_H */
