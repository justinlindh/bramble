#include "packet.h"

/* Big-endian helpers */
static inline void put_be16(uint8_t* buf, uint16_t v) {
    buf[0] = (uint8_t)(v >> 8);
    buf[1] = (uint8_t)(v);
}
static inline void put_be32(uint8_t* buf, uint32_t v) {
    buf[0] = (uint8_t)(v >> 24);
    buf[1] = (uint8_t)(v >> 16);
    buf[2] = (uint8_t)(v >> 8);
    buf[3] = (uint8_t)(v);
}
static inline uint16_t get_be16(const uint8_t* buf) {
    return (uint16_t)((uint16_t)buf[0] << 8 | buf[1]);
}
static inline uint32_t get_be32(const uint8_t* buf) {
    return (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 |
           (uint32_t)buf[3];
}

/* Header */
esp_err_t bramble_header_serialize(const bramble_header_t* h, uint8_t* buf, size_t len) {
    if (len < HEADER_SIZE)
        return ESP_ERR_INVALID_SIZE;
    buf[0] = h->version;
    buf[1] = h->type;
    buf[2] = h->flags;
    buf[3] = h->hop_limit;
    put_be32(buf + 4, h->dest_addr);
    put_be32(buf + 8, h->packet_id);
    return ESP_OK;
}

esp_err_t bramble_header_deserialize(bramble_header_t* h, const uint8_t* buf, size_t len) {
    if (len < HEADER_SIZE)
        return ESP_ERR_INVALID_SIZE;
    h->version = buf[0];
    h->type = buf[1];
    h->flags = buf[2];
    h->hop_limit = buf[3];
    h->dest_addr = get_be32(buf + 4);
    h->packet_id = get_be32(buf + 8);
    return ESP_OK;
}

bool bramble_header_is_supported_version(const bramble_header_t* h) {
    return h && h->version == BRAMBLE_VERSION;
}

esp_err_t bramble_header_build_aad(const bramble_header_t* h, uint8_t* buf, size_t len) {
    esp_err_t r = bramble_header_serialize(h, buf, len);
    if (r != ESP_OK)
        return r;
    buf[3] = 0; /* hop_limit: relay-mutated in flight, excluded from authentication */
    return ESP_OK;
}

esp_err_t bramble_build_aead_aad(const bramble_header_t* h, uint32_t src_addr, uint8_t* buf,
                                 size_t len) {
    if (len < HEADER_SIZE + 4)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t err = bramble_header_build_aad(h, buf, HEADER_SIZE);
    if (err != ESP_OK)
        return err;
    buf[HEADER_SIZE + 0] = (uint8_t)(src_addr & 0xFF);
    buf[HEADER_SIZE + 1] = (uint8_t)((src_addr >> 8) & 0xFF);
    buf[HEADER_SIZE + 2] = (uint8_t)((src_addr >> 16) & 0xFF);
    buf[HEADER_SIZE + 3] = (uint8_t)((src_addr >> 24) & 0xFF);
    return ESP_OK;
}

/* Macro for body offset */
#define B (HEADER_SIZE)

/* ACK (22 bytes) */
esp_err_t bramble_ack_serialize(const bramble_ack_t* p, uint8_t* buf, size_t len) {
    uint8_t hops = p->hop_count > ACK_MAX_HOPS ? ACK_MAX_HOPS : p->hop_count;
    size_t need = ACK_BASE_SIZE + hops * 4;
    if (len < need)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    put_be32(buf + B, p->src_addr);
    put_be32(buf + B + 4, p->ack_packet_id);
    buf[B + 8] = p->ack_flags;
    buf[B + 9] = (uint8_t)p->rssi_at_dest;
    buf[B + 10] = hops;
    /* NEW-SEC-8: auth_hmac at a fixed, hop_count-independent offset,
     * before relay_path. */
    memcpy(buf + B + 11, p->auth_hmac, 8);
    for (int i = 0; i < hops; i++) {
        put_be32(buf + B + 19 + i * 4, p->relay_path[i]);
    }
    return ESP_OK;
}

size_t bramble_ack_wire_size(const bramble_ack_t* p) {
    uint8_t hops = p->hop_count > ACK_MAX_HOPS ? ACK_MAX_HOPS : p->hop_count;
    return ACK_BASE_SIZE + hops * 4;
}

esp_err_t bramble_ack_deserialize(bramble_ack_t* p, const uint8_t* buf, size_t len) {
    if (len < ACK_BASE_SIZE)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    p->src_addr = get_be32(buf + B);
    p->ack_packet_id = get_be32(buf + B + 4);
    p->ack_flags = buf[B + 8];
    p->rssi_at_dest = (int8_t)buf[B + 9];
    p->hop_count = buf[B + 10];
    /* NEW-SEC-8: auth_hmac at a fixed offset, read BEFORE relay_path and
     * independent of hop_count, so a verifier never has to trust the
     * unauthenticated hop_count to locate the tag. */
    memcpy(p->auth_hmac, buf + B + 11, 8);
    if (p->hop_count > ACK_MAX_HOPS)
        p->hop_count = ACK_MAX_HOPS;
    /* Read as many hops as available in buffer */
    for (int i = 0; i < p->hop_count && (size_t)(B + 19 + (i + 1) * 4) <= len; i++) {
        p->relay_path[i] = get_be32(buf + B + 19 + i * 4);
    }
    return ESP_OK;
}

/* RREQ (26 bytes) */
esp_err_t bramble_rreq_serialize(const bramble_rreq_t* p, uint8_t* buf, size_t len) {
    if (len < RREQ_SIZE)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    put_be32(buf + B, p->query_id);
    put_be32(buf + B + 4, p->encrypted_source);
    buf[B + 8] = p->hop_count;
    buf[B + 9] = p->metric;
    put_be32(buf + B + 10, p->prev_hop);
    put_be32(buf + B + 14, p->rreq_salt);
    return ESP_OK;
}
esp_err_t bramble_rreq_deserialize(bramble_rreq_t* p, const uint8_t* buf, size_t len) {
    if (len < RREQ_SIZE)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    p->query_id = get_be32(buf + B);
    p->encrypted_source = get_be32(buf + B + 4);
    p->hop_count = buf[B + 8];
    p->metric = buf[B + 9];
    p->prev_hop = get_be32(buf + B + 10);
    p->rreq_salt = get_be32(buf + B + 14);
    return ESP_OK;
}

/* RREP (30 bytes) */
esp_err_t bramble_rrep_serialize(const bramble_rrep_t* p, uint8_t* buf, size_t len) {
    if (len < RREP_SIZE)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    put_be32(buf + B, p->query_id);
    put_be32(buf + B + 4, p->src_addr);
    put_be32(buf + B + 8, p->next_hop);
    buf[B + 12] = p->hop_count;
    buf[B + 13] = p->route_metric;
    memcpy(buf + B + 14, p->auth_hmac, 8);
    return ESP_OK;
}
esp_err_t bramble_rrep_deserialize(bramble_rrep_t* p, const uint8_t* buf, size_t len) {
    if (len < RREP_SIZE)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    p->query_id = get_be32(buf + B);
    p->src_addr = get_be32(buf + B + 4);
    p->next_hop = get_be32(buf + B + 8);
    p->hop_count = buf[B + 12];
    p->route_metric = buf[B + 13];
    memcpy(p->auth_hmac, buf + B + 14, 8);
    return ESP_OK;
}

/* RERR (24 bytes) */
esp_err_t bramble_rerr_serialize(const bramble_rerr_t* p, uint8_t* buf, size_t len) {
    if (len < RERR_SIZE)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    put_be32(buf + B, p->reporter_addr);
    put_be32(buf + B + 4, p->broken_dest);
    put_be32(buf + B + 8, p->broken_next_hop);
    memcpy(buf + B + 12, p->auth_hmac, 8);
    return ESP_OK;
}
esp_err_t bramble_rerr_deserialize(bramble_rerr_t* p, const uint8_t* buf, size_t len) {
    if (len < RERR_SIZE)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    p->reporter_addr = get_be32(buf + B);
    p->broken_dest = get_be32(buf + B + 4);
    p->broken_next_hop = get_be32(buf + B + 8);
    memcpy(p->auth_hmac, buf + B + 12, 8);
    return ESP_OK;
}

/* BEACON (40 bytes) */
esp_err_t bramble_beacon_serialize(const bramble_beacon_t* p, uint8_t* buf, size_t len) {
    uint8_t nlen = p->name_len > BEACON_NAME_MAX ? BEACON_NAME_MAX : p->name_len;
    size_t need = BEACON_SIZE + (nlen > 0 ? 1 + nlen : 0);
    if (len < need)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    put_be32(buf + B, p->src_addr);
    put_be32(buf + B + 4, p->pubkey_hash);
    put_be16(buf + B + 8, p->uptime_min);
    buf[B + 10] = p->battery_pct;
    buf[B + 11] = p->tx_queue_depth;
    buf[B + 12] = p->neighbor_count;
    buf[B + 13] = p->flags;
    put_be32(buf + B + 14, p->network_time);
    put_be16(buf + B + 18, p->time_confidence);
    memcpy(buf + B + 20, p->auth_hmac, 16);
    if (nlen > 0) {
        buf[BEACON_SIZE] = nlen;
        memcpy(buf + BEACON_SIZE + 1, p->name, nlen);
    }
    return ESP_OK;
}

size_t bramble_beacon_wire_size(const bramble_beacon_t* p) {
    uint8_t nlen = p->name_len > BEACON_NAME_MAX ? BEACON_NAME_MAX : p->name_len;
    return BEACON_SIZE + (nlen > 0 ? 1 + nlen : 0);
}

esp_err_t bramble_beacon_deserialize(bramble_beacon_t* p, const uint8_t* buf, size_t len) {
    if (len < BEACON_SIZE)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    p->src_addr = get_be32(buf + B);
    p->pubkey_hash = get_be32(buf + B + 4);
    p->uptime_min = get_be16(buf + B + 8);
    p->battery_pct = buf[B + 10];
    p->tx_queue_depth = buf[B + 11];
    p->neighbor_count = buf[B + 12];
    p->flags = buf[B + 13];
    p->network_time = get_be32(buf + B + 14);
    p->time_confidence = get_be16(buf + B + 18);
    memcpy(p->auth_hmac, buf + B + 20, 16);
    /* Optional name after fixed fields */
    p->name_len = 0;
    p->name[0] = '\0';
    if (len > BEACON_SIZE) {
        p->name_len = buf[BEACON_SIZE];
        if (p->name_len > BEACON_NAME_MAX)
            p->name_len = BEACON_NAME_MAX;
        if (len >= (size_t)(BEACON_SIZE + 1 + p->name_len)) {
            memcpy(p->name, buf + BEACON_SIZE + 1, p->name_len);
        }
        p->name[p->name_len] = '\0';
    }
    return ESP_OK;
}

/* KEY_EXCHANGE (69 bytes) */
esp_err_t bramble_key_exchange_serialize(const bramble_key_exchange_t* p, uint8_t* buf,
                                         size_t len) {
    if (len < KEY_EXCHANGE_SIZE)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    put_be32(buf + B, p->src_addr);
    memcpy(buf + B + 4, p->ephemeral_pubkey, 32);
    memcpy(buf + B + 36, p->long_term_pubkey, 32);
    buf[B + 68] = p->key_id;
    buf[B + 69] = p->ke_type;
    memcpy(buf + B + 70, p->auth_tag, 16);
    return ESP_OK;
}
esp_err_t bramble_key_exchange_deserialize(bramble_key_exchange_t* p, const uint8_t* buf,
                                           size_t len) {
    if (len < KEY_EXCHANGE_SIZE)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    p->src_addr = get_be32(buf + B);
    memcpy(p->ephemeral_pubkey, buf + B + 4, 32);
    memcpy(p->long_term_pubkey, buf + B + 36, 32);
    p->key_id = buf[B + 68];
    p->ke_type = buf[B + 69];
    memcpy(p->auth_tag, buf + B + 70, 16);
    return ESP_OK;
}

/* DELIVERY_RECEIPT (22-54 bytes) */
esp_err_t bramble_delivery_receipt_serialize(const bramble_delivery_receipt_t* p, uint8_t* buf,
                                             size_t len) {
    size_t needed = DELIVERY_RECEIPT_MIN_SIZE + (size_t)p->hop_count * 4;
    if (len < needed)
        return ESP_ERR_INVALID_SIZE;
    if (p->hop_count > DELIVERY_RECEIPT_MAX_HOPS)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_serialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    put_be32(buf + B, p->src_addr);
    put_be32(buf + B + 4, p->orig_packet_id);
    buf[B + 8] = p->hop_count;
    buf[B + 9] = p->total_latency;
    /* NEW-SEC-8: auth_hmac at a fixed, hop_count-independent offset,
     * before relay_path. */
    memcpy(buf + B + 10, p->auth_hmac, 8);
    for (uint8_t i = 0; i < p->hop_count; i++) {
        put_be32(buf + B + 18 + i * 4, p->relay_path[i]);
    }
    return ESP_OK;
}
esp_err_t bramble_delivery_receipt_deserialize(bramble_delivery_receipt_t* p, const uint8_t* buf,
                                               size_t len) {
    if (len < DELIVERY_RECEIPT_MIN_SIZE)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t r = bramble_header_deserialize(&p->header, buf, len);
    if (r != ESP_OK)
        return r;
    p->src_addr = get_be32(buf + B);
    p->orig_packet_id = get_be32(buf + B + 4);
    p->hop_count = buf[B + 8];
    p->total_latency = buf[B + 9];
    /* NEW-SEC-8: read at a fixed offset, before validating/using
     * hop_count, so a verifier never has to trust the unauthenticated
     * hop_count to locate the tag. */
    memcpy(p->auth_hmac, buf + B + 10, 8);
    if (p->hop_count > DELIVERY_RECEIPT_MAX_HOPS)
        return ESP_ERR_INVALID_SIZE;
    size_t needed = DELIVERY_RECEIPT_MIN_SIZE + (size_t)p->hop_count * 4;
    if (len < needed)
        return ESP_ERR_INVALID_SIZE;
    for (uint8_t i = 0; i < p->hop_count; i++) {
        p->relay_path[i] = get_be32(buf + B + 18 + i * 4);
    }
    return ESP_OK;
}
