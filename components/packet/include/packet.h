#ifndef BRAMBLE_PACKET_H
#define BRAMBLE_PACKET_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
#include "esp_stubs.h"
#endif

/* Protocol version */
#define BRAMBLE_VERSION 1

/* Packet types */
#define PKT_TYPE_ACK 0x01
#define PKT_TYPE_RREQ 0x02
#define PKT_TYPE_RREP 0x03
#define PKT_TYPE_RERR 0x04
#define PKT_TYPE_BEACON 0x05
#define PKT_TYPE_KEY_EXCHANGE 0x06
#define PKT_TYPE_DELIVERY_RECEIPT 0x07
#define PKT_TYPE_DATA 0x0A
#define PKT_TYPE_STORE_REQUEST 0x0B
#define PKT_TYPE_STORE_ACK 0x0C
#define PKT_TYPE_MAILBOX_DELIVERY 0x0D
#define PKT_TYPE_MAILBOX_QUERY 0x0E
#define PKT_TYPE_PROBE 0x12     /* Network reachability probe */
#define PKT_TYPE_PROBE_ACK 0x13 /* Probe acknowledgement */
#define PKT_TYPE_LOCATION 0x14  /* Location share */

#define BEACON_FLAG_MAILBOX 0x01 /* Node willing to store messages */

/* Buffer sizes */
#define BRAMBLE_MAX_PACKET_SIZE 256

/* Flag bits */
#define FLAG_TIER_SHIFT 6
#define FLAG_TIER_MASK 0xC0
#define FLAG_ACK_REQ (1 << 5)
#define FLAG_RECEIPT (1 << 4)
#define FLAG_CHANNEL (1 << 3)
#define FLAG_ENCRYPT (1 << 2)
#define FLAG_FRAG_MASK 0x03

/* Sizes */
#define HEADER_SIZE 12
#define ACK_BASE_SIZE                                                                              \
    23 /* header(12) + src(4) + ack_pkt_id(4) + flags(1) + rssi(1) + hop_count(1) */
#define ACK_MAX_HOPS 8
#define ACK_MAX_SIZE (ACK_BASE_SIZE + ACK_MAX_HOPS * 4) /* 23 + 32 = 55 */
#define ACK_SIZE ACK_BASE_SIZE                          /* backward compat for min size checks */
#define RREQ_SIZE 30
#define RREP_SIZE 34
#define RERR_SIZE 24
#define BEACON_SIZE 48
#define KEY_EXCHANGE_SIZE 101
#define DELIVERY_RECEIPT_MIN_SIZE 22
#define DELIVERY_RECEIPT_MAX_SIZE 54

#define DELIVERY_RECEIPT_MAX_HOPS 8

/* Common header (12 bytes) */
typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint8_t hop_limit;
    uint32_t dest_addr;
    uint32_t packet_id;
} bramble_header_t;

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint32_t ack_packet_id;
    uint8_t ack_flags;
    int8_t rssi_at_dest;
    uint8_t hop_count;                 /* number of addresses in relay_path */
    uint32_t relay_path[ACK_MAX_HOPS]; /* hop trail: [dest, relay1, relay2, ...] */
} bramble_ack_t;

typedef struct {
    bramble_header_t header;
    uint32_t query_id;
    uint32_t encrypted_source;
    uint8_t hop_count;
    uint8_t metric;
    uint32_t prev_hop;
    uint32_t rreq_salt;
} bramble_rreq_t;

typedef struct {
    bramble_header_t header;
    uint32_t query_id;
    uint32_t src_addr;
    uint32_t next_hop;
    uint8_t hop_count;
    uint8_t route_metric;
    uint8_t auth_hmac[8];
} bramble_rrep_t;

typedef struct {
    bramble_header_t header;
    uint32_t reporter_addr;
    uint32_t broken_dest;
    uint32_t broken_next_hop;
} bramble_rerr_t;

#define BEACON_NAME_MAX 16

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint32_t pubkey_hash;
    uint16_t uptime_min;
    uint8_t battery_pct;
    uint8_t tx_queue_depth;
    uint8_t neighbor_count;
    uint8_t flags;
    uint32_t network_time;
    uint16_t time_confidence;
    uint8_t auth_hmac[16];
    /* Optional: node name (appended after fixed fields) */
    uint8_t name_len;
    char name[BEACON_NAME_MAX + 1];
} bramble_beacon_t;

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint8_t ephemeral_pubkey[32];
    uint8_t long_term_pubkey[32];
    uint8_t key_id;
    uint8_t ke_type;
    uint8_t auth_tag[16];
} bramble_key_exchange_t;

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint32_t orig_packet_id;
    uint8_t hop_count;
    uint8_t total_latency;
    uint32_t relay_path[DELIVERY_RECEIPT_MAX_HOPS];
} bramble_delivery_receipt_t;

/* Serialize/deserialize functions. Return ESP_OK or ESP_ERR_INVALID_SIZE. */
esp_err_t bramble_header_serialize(const bramble_header_t* h, uint8_t* buf, size_t len);
esp_err_t bramble_header_deserialize(bramble_header_t* h, const uint8_t* buf, size_t len);

/*
 * Build the AES-GCM AAD for an encrypted DATA packet: the serialized header
 * with the hop_limit byte zeroed.
 *
 * Invariant: hop_limit is the only header field a relay mutates in flight
 * (forward_data_packet decrements it before retransmitting), so it must be
 * excluded from authentication or every forwarded packet fails the
 * destination's tag check. All other fields (version, type, flags,
 * dest_addr, packet_id) stay bound. If a relay ever needs to mutate another
 * header field, mask it here so the encrypt and decrypt paths cannot diverge.
 *
 * Both endpoints MUST use this helper: the originator when encrypting
 * (send_data_packet) and the destination when decrypting (handle_data).
 */
esp_err_t bramble_header_build_aad(const bramble_header_t* h, uint8_t* buf, size_t len);

esp_err_t bramble_ack_serialize(const bramble_ack_t* p, uint8_t* buf, size_t len);
esp_err_t bramble_ack_deserialize(bramble_ack_t* p, const uint8_t* buf, size_t len);
size_t bramble_ack_wire_size(const bramble_ack_t* p);

esp_err_t bramble_rreq_serialize(const bramble_rreq_t* p, uint8_t* buf, size_t len);
esp_err_t bramble_rreq_deserialize(bramble_rreq_t* p, const uint8_t* buf, size_t len);

esp_err_t bramble_rrep_serialize(const bramble_rrep_t* p, uint8_t* buf, size_t len);
esp_err_t bramble_rrep_deserialize(bramble_rrep_t* p, const uint8_t* buf, size_t len);

esp_err_t bramble_rerr_serialize(const bramble_rerr_t* p, uint8_t* buf, size_t len);
esp_err_t bramble_rerr_deserialize(bramble_rerr_t* p, const uint8_t* buf, size_t len);

esp_err_t bramble_beacon_serialize(const bramble_beacon_t* p, uint8_t* buf, size_t len);
esp_err_t bramble_beacon_deserialize(bramble_beacon_t* p, const uint8_t* buf, size_t len);
size_t bramble_beacon_wire_size(const bramble_beacon_t* p);

esp_err_t bramble_key_exchange_serialize(const bramble_key_exchange_t* p, uint8_t* buf, size_t len);
esp_err_t bramble_key_exchange_deserialize(bramble_key_exchange_t* p, const uint8_t* buf,
                                           size_t len);

esp_err_t bramble_delivery_receipt_serialize(const bramble_delivery_receipt_t* p, uint8_t* buf,
                                             size_t len);
esp_err_t bramble_delivery_receipt_deserialize(bramble_delivery_receipt_t* p, const uint8_t* buf,
                                               size_t len);

#endif /* BRAMBLE_PACKET_H */
