#include "packet.h"

/* Big-endian helpers */
static inline void put_be16(uint8_t *buf, uint16_t v) {
    buf[0] = (uint8_t)(v >> 8);
    buf[1] = (uint8_t)(v);
}
static inline void put_be32(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)(v >> 24);
    buf[1] = (uint8_t)(v >> 16);
    buf[2] = (uint8_t)(v >> 8);
    buf[3] = (uint8_t)(v);
}
static inline uint16_t get_be16(const uint8_t *buf) {
    return (uint16_t)((uint16_t)buf[0] << 8 | buf[1]);
}
static inline uint32_t get_be32(const uint8_t *buf) {
    return (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 |
           (uint32_t)buf[2] << 8  | (uint32_t)buf[3];
}

/* Header */
esp_err_t bramble_header_serialize(const bramble_header_t *h, uint8_t *buf, size_t len) {
    if (len < HEADER_SIZE) return ESP_ERR_INVALID_SIZE;
    buf[0] = h->version;
    buf[1] = h->type;
    buf[2] = h->flags;
    buf[3] = h->hop_limit;
    put_be32(buf + 4, h->dest_addr);
    put_be32(buf + 8, h->packet_id);
    return ESP_OK;
}

esp_err_t bramble_header_deserialize(bramble_header_t *h, const uint8_t *buf, size_t len) {
    if (len < HEADER_SIZE) return ESP_ERR_INVALID_SIZE;
    h->version   = buf[0];
    h->type      = buf[1];
    h->flags     = buf[2];
    h->hop_limit = buf[3];
    h->dest_addr = get_be32(buf + 4);
    h->packet_id = get_be32(buf + 8);
    return ESP_OK;
}

/* Macro for body offset */
#define B (HEADER_SIZE)

/* ACK (22 bytes) */
esp_err_t bramble_ack_serialize(const bramble_ack_t *p, uint8_t *buf, size_t len) {
    if (len < ACK_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    put_be32(buf + B, p->src_addr);
    put_be32(buf + B + 4, p->ack_packet_id);
    buf[B + 8] = p->ack_flags;
    buf[B + 9] = (uint8_t)p->rssi_at_dest;
    return ESP_OK;
}
esp_err_t bramble_ack_deserialize(bramble_ack_t *p, const uint8_t *buf, size_t len) {
    if (len < ACK_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    p->src_addr      = get_be32(buf + B);
    p->ack_packet_id = get_be32(buf + B + 4);
    p->ack_flags     = buf[B + 8];
    p->rssi_at_dest  = (int8_t)buf[B + 9];
    return ESP_OK;
}

/* RREQ (26 bytes) */
esp_err_t bramble_rreq_serialize(const bramble_rreq_t *p, uint8_t *buf, size_t len) {
    if (len < RREQ_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    put_be32(buf + B, p->query_id);
    put_be32(buf + B + 4, p->encrypted_source);
    buf[B + 8] = p->hop_count;
    buf[B + 9] = p->metric;
    put_be32(buf + B + 10, p->prev_hop);
    put_be32(buf + B + 14, p->rreq_salt);
    return ESP_OK;
}
esp_err_t bramble_rreq_deserialize(bramble_rreq_t *p, const uint8_t *buf, size_t len) {
    if (len < RREQ_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    p->query_id         = get_be32(buf + B);
    p->encrypted_source = get_be32(buf + B + 4);
    p->hop_count        = buf[B + 8];
    p->metric           = buf[B + 9];
    p->prev_hop         = get_be32(buf + B + 10);
    p->rreq_salt        = get_be32(buf + B + 14);
    return ESP_OK;
}

/* RREP (30 bytes) */
esp_err_t bramble_rrep_serialize(const bramble_rrep_t *p, uint8_t *buf, size_t len) {
    if (len < RREP_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    put_be32(buf + B, p->query_id);
    put_be32(buf + B + 4, p->src_addr);
    put_be32(buf + B + 8, p->next_hop);
    buf[B + 12] = p->hop_count;
    buf[B + 13] = p->route_metric;
    memcpy(buf + B + 14, p->auth_hmac, 8);
    return ESP_OK;
}
esp_err_t bramble_rrep_deserialize(bramble_rrep_t *p, const uint8_t *buf, size_t len) {
    if (len < RREP_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    p->query_id     = get_be32(buf + B);
    p->src_addr     = get_be32(buf + B + 4);
    p->next_hop     = get_be32(buf + B + 8);
    p->hop_count    = buf[B + 12];
    p->route_metric = buf[B + 13];
    memcpy(p->auth_hmac, buf + B + 14, 8);
    return ESP_OK;
}

/* RERR (24 bytes) */
esp_err_t bramble_rerr_serialize(const bramble_rerr_t *p, uint8_t *buf, size_t len) {
    if (len < RERR_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    put_be32(buf + B, p->reporter_addr);
    put_be32(buf + B + 4, p->broken_dest);
    put_be32(buf + B + 8, p->broken_next_hop);
    return ESP_OK;
}
esp_err_t bramble_rerr_deserialize(bramble_rerr_t *p, const uint8_t *buf, size_t len) {
    if (len < RERR_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    p->reporter_addr  = get_be32(buf + B);
    p->broken_dest    = get_be32(buf + B + 4);
    p->broken_next_hop = get_be32(buf + B + 8);
    return ESP_OK;
}

/* BEACON (40 bytes) */
esp_err_t bramble_beacon_serialize(const bramble_beacon_t *p, uint8_t *buf, size_t len) {
    if (len < BEACON_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    put_be32(buf + B, p->src_addr);
    put_be32(buf + B + 4, p->pubkey_hash);
    put_be16(buf + B + 8, p->uptime_min);
    buf[B + 10] = p->battery_pct;
    buf[B + 11] = p->tx_queue_depth;
    buf[B + 12] = p->neighbor_count;
    buf[B + 13] = p->flags;
    put_be32(buf + B + 14, p->network_time);
    put_be16(buf + B + 18, p->time_confidence);
    memcpy(buf + B + 20, p->auth_hmac, 12);
    return ESP_OK;
}
esp_err_t bramble_beacon_deserialize(bramble_beacon_t *p, const uint8_t *buf, size_t len) {
    if (len < BEACON_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    p->src_addr        = get_be32(buf + B);
    p->pubkey_hash     = get_be32(buf + B + 4);
    p->uptime_min      = get_be16(buf + B + 8);
    p->battery_pct     = buf[B + 10];
    p->tx_queue_depth  = buf[B + 11];
    p->neighbor_count  = buf[B + 12];
    p->flags           = buf[B + 13];
    p->network_time    = get_be32(buf + B + 14);
    p->time_confidence = get_be16(buf + B + 18);
    memcpy(p->auth_hmac, buf + B + 20, 12);
    return ESP_OK;
}

/* KEY_EXCHANGE (69 bytes) */
esp_err_t bramble_key_exchange_serialize(const bramble_key_exchange_t *p, uint8_t *buf, size_t len) {
    if (len < KEY_EXCHANGE_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    put_be32(buf + B, p->src_addr);
    memcpy(buf + B + 4, p->ephemeral_pubkey, 32);
    memcpy(buf + B + 36, p->long_term_pubkey, 32);
    buf[B + 68] = p->key_id;
    buf[B + 69] = p->ke_type;
    memcpy(buf + B + 70, p->auth_tag, 16);
    return ESP_OK;
}
esp_err_t bramble_key_exchange_deserialize(bramble_key_exchange_t *p, const uint8_t *buf, size_t len) {
    if (len < KEY_EXCHANGE_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    p->src_addr = get_be32(buf + B);
    memcpy(p->ephemeral_pubkey, buf + B + 4, 32);
    memcpy(p->long_term_pubkey, buf + B + 36, 32);
    p->key_id  = buf[B + 68];
    p->ke_type = buf[B + 69];
    memcpy(p->auth_tag, buf + B + 70, 16);
    return ESP_OK;
}

/* DELIVERY_RECEIPT (22-54 bytes) */
esp_err_t bramble_delivery_receipt_serialize(const bramble_delivery_receipt_t *p, uint8_t *buf, size_t len) {
    size_t needed = DELIVERY_RECEIPT_MIN_SIZE + (size_t)p->hop_count * 4;
    if (len < needed) return ESP_ERR_INVALID_SIZE;
    if (p->hop_count > DELIVERY_RECEIPT_MAX_HOPS) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    put_be32(buf + B, p->src_addr);
    put_be32(buf + B + 4, p->orig_packet_id);
    buf[B + 8] = p->hop_count;
    buf[B + 9] = p->total_latency;
    for (uint8_t i = 0; i < p->hop_count; i++) {
        put_be32(buf + B + 10 + i * 4, p->relay_path[i]);
    }
    return ESP_OK;
}
esp_err_t bramble_delivery_receipt_deserialize(bramble_delivery_receipt_t *p, const uint8_t *buf, size_t len) {
    if (len < DELIVERY_RECEIPT_MIN_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    p->src_addr       = get_be32(buf + B);
    p->orig_packet_id = get_be32(buf + B + 4);
    p->hop_count      = buf[B + 8];
    p->total_latency  = buf[B + 9];
    if (p->hop_count > DELIVERY_RECEIPT_MAX_HOPS) return ESP_ERR_INVALID_SIZE;
    size_t needed = DELIVERY_RECEIPT_MIN_SIZE + (size_t)p->hop_count * 4;
    if (len < needed) return ESP_ERR_INVALID_SIZE;
    for (uint8_t i = 0; i < p->hop_count; i++) {
        p->relay_path[i] = get_be32(buf + B + 10 + i * 4);
    }
    return ESP_OK;
}

/* CONGESTION (20 bytes) */
esp_err_t bramble_congestion_serialize(const bramble_congestion_t *p, uint8_t *buf, size_t len) {
    if (len < CONGESTION_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    put_be32(buf + B, p->src_addr);
    buf[B + 4] = p->congestion_level;
    buf[B + 5] = p->queue_depth;
    put_be16(buf + B + 6, p->est_clear_time);
    return ESP_OK;
}
esp_err_t bramble_congestion_deserialize(bramble_congestion_t *p, const uint8_t *buf, size_t len) {
    if (len < CONGESTION_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    p->src_addr         = get_be32(buf + B);
    p->congestion_level = buf[B + 4];
    p->queue_depth      = buf[B + 5];
    p->est_clear_time   = get_be16(buf + B + 6);
    return ESP_OK;
}

/* TIME_SYNC (24 bytes) */
esp_err_t bramble_time_sync_serialize(const bramble_time_sync_t *p, uint8_t *buf, size_t len) {
    if (len < TIME_SYNC_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    put_be32(buf + B, p->src_addr);
    put_be32(buf + B + 4, p->timestamp);
    put_be16(buf + B + 8, p->confidence_ms);
    buf[B + 10] = p->stratum;
    buf[B + 11] = p->sequence;
    return ESP_OK;
}
esp_err_t bramble_time_sync_deserialize(bramble_time_sync_t *p, const uint8_t *buf, size_t len) {
    if (len < TIME_SYNC_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK) return r;
    p->src_addr       = get_be32(buf + B);
    p->timestamp      = get_be32(buf + B + 4);
    p->confidence_ms  = get_be16(buf + B + 8);
    p->stratum        = buf[B + 10];
    p->sequence       = buf[B + 11];
    return ESP_OK;
}
