#include "reliability.h"
#include <string.h>
#include <stdlib.h>

uint8_t tier_max_retries(uint8_t tier) {
    switch (tier) {
    case MSG_TIER_BROADCAST:
        return 0;
    case MSG_TIER_NORMAL:
        return 3;
    case MSG_TIER_CRITICAL:
        return 8;
    default:
        return 0;
    }
}

uint32_t tier_base_delay_ms(uint8_t tier) {
    switch (tier) {
    case MSG_TIER_NORMAL:
        return 2000;
    case MSG_TIER_CRITICAL:
        return 3000;
    default:
        return 0;
    }
}

uint8_t msg_tier_for_send(bool is_key_exchange) {
    return is_key_exchange ? MSG_TIER_CRITICAL : MSG_TIER_NORMAL;
}

void pending_ack_init(pending_ack_table_t* table) { memset(table, 0, sizeof(*table)); }

int pending_ack_add(pending_ack_table_t* table, uint32_t packet_id, uint32_t dest_addr,
                    uint8_t tier, const uint8_t* packet, uint16_t len, uint32_t now_ms) {
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!table->entries[i].active) {
            pending_ack_t* e = &table->entries[i];
            e->packet_id = packet_id;
            e->dest_addr = dest_addr;
            e->tier = tier;
            e->attempt = 0;
            e->max_attempts = tier_max_retries(tier);
            /* Clamp the recorded length to what actually fits in packet_data:
             * packet_len must never exceed the bytes copied, or every
             * retransmit consumer (mesh_tx, the simulator bridge) reads past
             * the buffer. Legitimate DATA frames are <= PENDING_ACK_MAX_FRAME
             * by construction; the clamp is a defensive invariant, not a
             * silent truncation of valid traffic. */
            uint16_t stored = len > sizeof(e->packet_data) ? (uint16_t)sizeof(e->packet_data) : len;
            e->packet_len = stored;
            if (stored > 0 && packet) {
                memcpy(e->packet_data, packet, stored);
            }
            e->next_retry_ms = now_ms + tier_base_delay_ms(tier);
            e->active = true;
            return i;
        }
    }
    return -1;
}

bool pending_ack_remove(pending_ack_table_t* table, uint32_t packet_id) {
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (table->entries[i].active && table->entries[i].packet_id == packet_id) {
            table->entries[i].active = false;
            return true;
        }
    }
    return false;
}

static uint32_t jitter_25(uint32_t base) {
    uint32_t quarter = base / 4;
    if (quarter == 0)
        return base;
    return base - quarter + (uint32_t)(rand() % (2 * quarter + 1));
}

void pending_ack_tick(pending_ack_table_t* table, uint32_t now_ms) {
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        pending_ack_t* e = &table->entries[i];
        if (!e->active)
            continue;
        if (now_ms >= e->next_retry_ms) {
            e->attempt++;
            if (e->attempt >= e->max_attempts) {
                e->active = false;
                continue;
            }
            uint32_t delay = tier_base_delay_ms(e->tier) << e->attempt;
            e->next_retry_ms = now_ms + jitter_25(delay);
        }
    }
}
