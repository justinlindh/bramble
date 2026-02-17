#ifndef BRAMBLE_CODING_H
#define BRAMBLE_CODING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CODING_MAX_PACKET_SIZE  256
#define CODING_RECEPTION_CACHE  32   /* recent packet IDs we've seen */
#define CODING_QUEUE_SIZE       8    /* packets queued for potential coding */
#define CODING_MAX_COMPONENTS   2    /* XOR of 2 packets only */
#define CODING_OPPORTUNITY_WINDOW_MS 500  /* time to wait for coding partner */

/* Reception report — what packet IDs a neighbor has seen */
typedef struct {
    uint32_t packet_ids[CODING_RECEPTION_CACHE];
    int count;
    int head;  /* circular buffer index */
} reception_cache_t;

/* Queued packet awaiting coding opportunity */
typedef struct {
    uint8_t data[CODING_MAX_PACKET_SIZE];
    uint16_t len;
    uint32_t packet_id;
    uint32_t dest_addr;      /* next-hop destination */
    uint32_t queued_at_ms;
    bool active;
} coding_queue_entry_t;

/* Coded packet header info */
typedef struct {
    uint32_t component_ids[CODING_MAX_COMPONENTS];
    uint16_t component_lens[CODING_MAX_COMPONENTS];
    int num_components;
} coded_header_t;

#define CODED_HEADER_SIZE  12
/* Header: num_components(1) + for each: packet_id(4) + orig_len(2) = 1 + 2*6 = 13 bytes */
#define CODED_HEADER_MAX_SIZE  (1 + CODING_MAX_COMPONENTS * 6)

/* Coding engine state */
typedef struct {
    reception_cache_t my_cache;                    /* packets I've seen/sent */
    coding_queue_entry_t queue[CODING_QUEUE_SIZE]; /* packets waiting for coding */
    /* Per-neighbor reception knowledge */
    struct {
        uint32_t neighbor_addr;
        reception_cache_t cache;
        bool active;
    } neighbor_knowledge[16];
    int neighbor_count;
} coding_engine_t;

/* Init */
void coding_init(coding_engine_t *engine);

/* Record that we've seen/sent a packet (for our own reception cache) */
void coding_record_packet(coding_engine_t *engine, uint32_t packet_id);

/* Record what a neighbor has seen (from piggybacked reception reports) */
void coding_record_neighbor_reception(coding_engine_t *engine, uint32_t neighbor_addr,
                                       const uint32_t *packet_ids, int count);

/* Check if a neighbor has a specific packet in their cache */
bool coding_neighbor_has_packet(const coding_engine_t *engine, uint32_t neighbor_addr,
                                 uint32_t packet_id);

/* Queue a packet for potential coding (instead of sending immediately) */
int coding_queue_packet(coding_engine_t *engine, const uint8_t *data, uint16_t len,
                        uint32_t packet_id, uint32_t dest_addr, uint32_t now_ms);

/* Check for coding opportunity — can any two queued packets be XOR-coded?
   Returns 0 if found (indices in idx_a/idx_b), -1 if no opportunity. */
int coding_find_opportunity(const coding_engine_t *engine, int *idx_a, int *idx_b);

/* XOR-encode two packets into a coded packet */
int coding_encode(const uint8_t *pkt_a, uint16_t len_a, uint32_t id_a,
                  const uint8_t *pkt_b, uint16_t len_b, uint32_t id_b,
                  uint8_t *coded_out, uint16_t *coded_len_out);

/* Decode a coded packet using a known component */
int coding_decode(const uint8_t *coded_data, uint16_t coded_len,
                  const coded_header_t *header,
                  const uint8_t *known_component, uint16_t known_len, uint32_t known_id,
                  uint8_t *decoded_out, uint16_t *decoded_len_out);

/* Serialize/deserialize coded header */
int coded_header_serialize(const coded_header_t *hdr, uint8_t *buf, size_t buf_len);
int coded_header_deserialize(const uint8_t *buf, size_t len, coded_header_t *hdr);

/* Flush expired entries from queue */
void coding_flush_expired(coding_engine_t *engine, uint32_t now_ms);

/* Check if we can decode a coded packet (do we have one of the components?) */
bool coding_can_decode(const coding_engine_t *engine, const coded_header_t *header,
                       uint32_t *known_id_out);

#endif
