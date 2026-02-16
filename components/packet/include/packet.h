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

/* Flag bits */
#define FLAG_TIER_SHIFT    6
#define FLAG_TIER_MASK     0xC0
#define FLAG_ACK_REQ       (1 << 5)
#define FLAG_RECEIPT       (1 << 4)
#define FLAG_CHANNEL       (1 << 3)
#define FLAG_ENCRYPT       (1 << 2)
#define FLAG_FRAG_MASK     0x03

/* Sizes */
#define HEADER_SIZE              12
#define ACK_SIZE                 22
#define RREQ_SIZE                26
#define RREP_SIZE                30
#define RERR_SIZE                24
#define BEACON_SIZE              36
#define KEY_EXCHANGE_SIZE        69
#define DELIVERY_RECEIPT_MIN_SIZE 22
#define DELIVERY_RECEIPT_MAX_SIZE 54
#define CONGESTION_SIZE          20
#define TIME_SYNC_SIZE           24

#define DELIVERY_RECEIPT_MAX_HOPS 8

/* Common header (12 bytes) */
typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint8_t  flags;
    uint8_t  hop_limit;
    uint32_t dest_addr;
    uint32_t packet_id;
} bramble_header_t;

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint32_t ack_packet_id;
    uint8_t  ack_flags;
    int8_t   rssi_at_dest;
} bramble_ack_t;

typedef struct {
    bramble_header_t header;
    uint32_t query_id;
    uint32_t encrypted_source;
    uint8_t  hop_count;
    uint8_t  metric;
    uint32_t prev_hop;
} bramble_rreq_t;

typedef struct {
    bramble_header_t header;
    uint32_t query_id;
    uint32_t src_addr;
    uint32_t next_hop;
    uint8_t  hop_count;
    uint8_t  route_metric;
    uint8_t  auth_hmac[4];
} bramble_rrep_t;

typedef struct {
    bramble_header_t header;
    uint32_t reporter_addr;
    uint32_t broken_dest;
    uint32_t broken_next_hop;
} bramble_rerr_t;

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint32_t pubkey_hash;
    uint16_t uptime_min;
    uint8_t  battery_pct;
    uint8_t  tx_queue_depth;
    uint8_t  neighbor_count;
    uint8_t  flags;
    uint32_t network_time;
    uint16_t time_confidence;
    uint8_t  auth_hmac[4];
} bramble_beacon_t;

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint8_t  ephemeral_pubkey[32];
    uint8_t  key_id;
    uint8_t  ke_type;
    uint8_t  auth_tag[16];
} bramble_key_exchange_t;

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint32_t orig_packet_id;
    uint8_t  hop_count;
    uint8_t  total_latency;
    uint32_t relay_path[DELIVERY_RECEIPT_MAX_HOPS];
} bramble_delivery_receipt_t;

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint8_t  congestion_level;
    uint8_t  queue_depth;
    uint16_t est_clear_time;
} bramble_congestion_t;

typedef struct {
    bramble_header_t header;
    uint32_t src_addr;
    uint32_t timestamp;
    uint16_t confidence_ms;
    uint8_t  stratum;
    uint8_t  sequence;
} bramble_time_sync_t;

/* Serialize/deserialize functions. Return ESP_OK or ESP_ERR_INVALID_SIZE. */
esp_err_t bramble_header_serialize(const bramble_header_t *h, uint8_t *buf, size_t len);
esp_err_t bramble_header_deserialize(bramble_header_t *h, const uint8_t *buf, size_t len);

esp_err_t bramble_ack_serialize(const bramble_ack_t *p, uint8_t *buf, size_t len);
esp_err_t bramble_ack_deserialize(bramble_ack_t *p, const uint8_t *buf, size_t len);

esp_err_t bramble_rreq_serialize(const bramble_rreq_t *p, uint8_t *buf, size_t len);
esp_err_t bramble_rreq_deserialize(bramble_rreq_t *p, const uint8_t *buf, size_t len);

esp_err_t bramble_rrep_serialize(const bramble_rrep_t *p, uint8_t *buf, size_t len);
esp_err_t bramble_rrep_deserialize(bramble_rrep_t *p, const uint8_t *buf, size_t len);

esp_err_t bramble_rerr_serialize(const bramble_rerr_t *p, uint8_t *buf, size_t len);
esp_err_t bramble_rerr_deserialize(bramble_rerr_t *p, const uint8_t *buf, size_t len);

esp_err_t bramble_beacon_serialize(const bramble_beacon_t *p, uint8_t *buf, size_t len);
esp_err_t bramble_beacon_deserialize(bramble_beacon_t *p, const uint8_t *buf, size_t len);

esp_err_t bramble_key_exchange_serialize(const bramble_key_exchange_t *p, uint8_t *buf, size_t len);
esp_err_t bramble_key_exchange_deserialize(bramble_key_exchange_t *p, const uint8_t *buf, size_t len);

esp_err_t bramble_delivery_receipt_serialize(const bramble_delivery_receipt_t *p, uint8_t *buf, size_t len);
esp_err_t bramble_delivery_receipt_deserialize(bramble_delivery_receipt_t *p, const uint8_t *buf, size_t len);

esp_err_t bramble_congestion_serialize(const bramble_congestion_t *p, uint8_t *buf, size_t len);
esp_err_t bramble_congestion_deserialize(bramble_congestion_t *p, const uint8_t *buf, size_t len);

esp_err_t bramble_time_sync_serialize(const bramble_time_sync_t *p, uint8_t *buf, size_t len);
esp_err_t bramble_time_sync_deserialize(bramble_time_sync_t *p, const uint8_t *buf, size_t len);

#endif /* BRAMBLE_PACKET_H */
