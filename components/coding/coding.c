#include "coding.h"
#include <string.h>

void coding_init(coding_engine_t* engine) { memset(engine, 0, sizeof(*engine)); }

void coding_record_packet(coding_engine_t* engine, uint32_t packet_id) {
    reception_cache_t* c = &engine->my_cache;
    c->packet_ids[c->head] = packet_id;
    c->head = (c->head + 1) % CODING_RECEPTION_CACHE;
    if (c->count < CODING_RECEPTION_CACHE) {
        c->count++;
    }
}

static bool cache_has_packet(const reception_cache_t* c, uint32_t packet_id) {
    for (int i = 0; i < c->count; i++) {
        int idx = (c->head - 1 - i + CODING_RECEPTION_CACHE) % CODING_RECEPTION_CACHE;
        if (c->packet_ids[idx] == packet_id)
            return true;
    }
    return false;
}

void coding_record_neighbor_reception(coding_engine_t* engine, uint32_t neighbor_addr,
                                      const uint32_t* packet_ids, int count) {
    /* Find or create neighbor entry */
    int ni = -1;
    for (int i = 0; i < engine->neighbor_count; i++) {
        if (engine->neighbor_knowledge[i].active &&
            engine->neighbor_knowledge[i].neighbor_addr == neighbor_addr) {
            ni = i;
            break;
        }
    }
    if (ni < 0) {
        if (engine->neighbor_count >= 16)
            return;
        ni = engine->neighbor_count++;
        engine->neighbor_knowledge[ni].neighbor_addr = neighbor_addr;
        engine->neighbor_knowledge[ni].active = true;
        memset(&engine->neighbor_knowledge[ni].cache, 0, sizeof(reception_cache_t));
    }

    reception_cache_t* c = &engine->neighbor_knowledge[ni].cache;
    for (int i = 0; i < count; i++) {
        if (!cache_has_packet(c, packet_ids[i])) {
            c->packet_ids[c->head] = packet_ids[i];
            c->head = (c->head + 1) % CODING_RECEPTION_CACHE;
            if (c->count < CODING_RECEPTION_CACHE)
                c->count++;
        }
    }
}

bool coding_neighbor_has_packet(const coding_engine_t* engine, uint32_t neighbor_addr,
                                uint32_t packet_id) {
    for (int i = 0; i < engine->neighbor_count; i++) {
        if (engine->neighbor_knowledge[i].active &&
            engine->neighbor_knowledge[i].neighbor_addr == neighbor_addr) {
            return cache_has_packet(&engine->neighbor_knowledge[i].cache, packet_id);
        }
    }
    return false;
}

int coding_queue_packet(coding_engine_t* engine, const uint8_t* data, uint16_t len,
                        uint32_t packet_id, uint32_t dest_addr, uint32_t now_ms) {
    if (len > CODING_MAX_PACKET_SIZE)
        return -1;
    for (int i = 0; i < CODING_QUEUE_SIZE; i++) {
        if (!engine->queue[i].active) {
            memcpy(engine->queue[i].data, data, len);
            engine->queue[i].len = len;
            engine->queue[i].packet_id = packet_id;
            engine->queue[i].dest_addr = dest_addr;
            engine->queue[i].queued_at_ms = now_ms;
            engine->queue[i].active = true;
            return 0;
        }
    }
    return -1; /* queue full */
}

int coding_find_opportunity(const coding_engine_t* engine, int* idx_a, int* idx_b) {
    /*
     * For packets A→X and B→Y, coding is possible if:
     *   - X has seen B's packet (so X can decode A from A⊕B)
     *   - Y has seen A's packet (so Y can decode B from A⊕B)
     */
    for (int i = 0; i < CODING_QUEUE_SIZE; i++) {
        if (!engine->queue[i].active)
            continue;
        for (int j = i + 1; j < CODING_QUEUE_SIZE; j++) {
            if (!engine->queue[j].active)
                continue;
            uint32_t dest_i = engine->queue[i].dest_addr;
            uint32_t dest_j = engine->queue[j].dest_addr;
            uint32_t id_i = engine->queue[i].packet_id;
            uint32_t id_j = engine->queue[j].packet_id;

            if (coding_neighbor_has_packet(engine, dest_i, id_j) &&
                coding_neighbor_has_packet(engine, dest_j, id_i)) {
                *idx_a = i;
                *idx_b = j;
                return 0;
            }
        }
    }
    return -1;
}

int coding_encode(const uint8_t* pkt_a, uint16_t len_a, uint32_t id_a, const uint8_t* pkt_b,
                  uint16_t len_b, uint32_t id_b, uint8_t* coded_out, uint16_t* coded_len_out) {
    /* Build header */
    coded_header_t hdr;
    hdr.num_components = 2;
    hdr.component_ids[0] = id_a;
    hdr.component_ids[1] = id_b;
    hdr.component_lens[0] = len_a;
    hdr.component_lens[1] = len_b;

    int hdr_len = coded_header_serialize(&hdr, coded_out, CODED_HEADER_MAX_SIZE);
    if (hdr_len < 0)
        return -1;

    /* XOR payloads */
    uint16_t max_len = len_a > len_b ? len_a : len_b;
    if ((uint16_t)hdr_len + max_len > CODING_MAX_PACKET_SIZE + CODED_HEADER_MAX_SIZE)
        return -1;

    uint8_t* xor_out = coded_out + hdr_len;
    for (uint16_t i = 0; i < max_len; i++) {
        uint8_t a = (i < len_a) ? pkt_a[i] : 0;
        uint8_t b = (i < len_b) ? pkt_b[i] : 0;
        xor_out[i] = a ^ b;
    }

    *coded_len_out = (uint16_t)(hdr_len + max_len);
    return 0;
}

int coding_decode(const uint8_t* coded_data, uint16_t coded_len, const coded_header_t* header,
                  const uint8_t* known_component, uint16_t known_len, uint32_t known_id,
                  uint8_t* decoded_out, uint16_t* decoded_len_out) {
    if (header->num_components != 2)
        return -1;

    /* Figure out which component is known and get the unknown's length */
    uint16_t unknown_len = 0;
    bool found = false;
    for (int i = 0; i < 2; i++) {
        if (header->component_ids[i] == known_id) {
            unknown_len = header->component_lens[1 - i];
            found = true;
            break;
        }
    }
    if (!found)
        return -1;

    /* XOR data starts after header */
    int hdr_size = 1 + header->num_components * 6;
    const uint8_t* xor_data = coded_data + hdr_size;
    uint16_t xor_len = coded_len - (uint16_t)hdr_size;

    for (uint16_t i = 0; i < unknown_len; i++) {
        uint8_t x = (i < xor_len) ? xor_data[i] : 0;
        uint8_t k = (i < known_len) ? known_component[i] : 0;
        decoded_out[i] = x ^ k;
    }

    *decoded_len_out = unknown_len;
    return 0;
}

int coded_header_serialize(const coded_header_t* hdr, uint8_t* buf, size_t buf_len) {
    size_t needed = 1 + (size_t)hdr->num_components * 6;
    if (buf_len < needed)
        return -1;

    buf[0] = (uint8_t)hdr->num_components;
    size_t off = 1;
    for (int i = 0; i < hdr->num_components; i++) {
        /* Little-endian packet_id (4 bytes) */
        buf[off++] = (uint8_t)(hdr->component_ids[i]);
        buf[off++] = (uint8_t)(hdr->component_ids[i] >> 8);
        buf[off++] = (uint8_t)(hdr->component_ids[i] >> 16);
        buf[off++] = (uint8_t)(hdr->component_ids[i] >> 24);
        /* Little-endian orig_len (2 bytes) */
        buf[off++] = (uint8_t)(hdr->component_lens[i]);
        buf[off++] = (uint8_t)(hdr->component_lens[i] >> 8);
    }
    return (int)off;
}

int coded_header_deserialize(const uint8_t* buf, size_t len, coded_header_t* hdr) {
    if (len < 1)
        return -1;
    hdr->num_components = buf[0];
    if (hdr->num_components > CODING_MAX_COMPONENTS)
        return -1;

    size_t needed = 1 + (size_t)hdr->num_components * 6;
    if (len < needed)
        return -1;

    size_t off = 1;
    for (int i = 0; i < hdr->num_components; i++) {
        hdr->component_ids[i] = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
                                ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
        off += 4;
        hdr->component_lens[i] = (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
        off += 2;
    }
    return (int)off;
}

void coding_flush_expired(coding_engine_t* engine, uint32_t now_ms) {
    for (int i = 0; i < CODING_QUEUE_SIZE; i++) {
        if (engine->queue[i].active &&
            (now_ms - engine->queue[i].queued_at_ms) > CODING_OPPORTUNITY_WINDOW_MS) {
            engine->queue[i].active = false;
        }
    }
}

bool coding_can_decode(const coding_engine_t* engine, const coded_header_t* header,
                       uint32_t* known_id_out) {
    for (int i = 0; i < header->num_components; i++) {
        if (cache_has_packet(&engine->my_cache, header->component_ids[i])) {
            *known_id_out = header->component_ids[i];
            return true;
        }
    }
    return false;
}
