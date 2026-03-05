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
            e->packet_len = len;
            if (len > 0 && packet) {
                memcpy(e->packet_data, packet, len > 222 ? 222 : len);
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

/* Flow control */

void flow_init(flow_control_t* fc) { memset(fc, 0, sizeof(*fc)); }

static flow_window_t* flow_find_or_create(flow_control_t* fc, uint32_t dest_addr) {
    for (int i = 0; i < MAX_FLOW_DESTINATIONS; i++) {
        if (fc->windows[i].active && fc->windows[i].dest_addr == dest_addr)
            return &fc->windows[i];
    }
    for (int i = 0; i < MAX_FLOW_DESTINATIONS; i++) {
        if (!fc->windows[i].active) {
            fc->windows[i].active = true;
            fc->windows[i].dest_addr = dest_addr;
            fc->windows[i].unacked = 0;
            fc->windows[i].window_size = FLOW_WINDOW_SIZE;
            fc->windows[i].success_counter = 0;
            return &fc->windows[i];
        }
    }
    return NULL;
}

bool flow_can_send(flow_control_t* fc, uint32_t dest_addr) {
    flow_window_t* w = flow_find_or_create(fc, dest_addr);
    if (!w)
        return false;
    return w->unacked < w->window_size;
}

void flow_on_send(flow_control_t* fc, uint32_t dest_addr) {
    flow_window_t* w = flow_find_or_create(fc, dest_addr);
    if (w)
        w->unacked++;
}

void flow_on_ack(flow_control_t* fc, uint32_t dest_addr) {
    flow_window_t* w = flow_find_or_create(fc, dest_addr);
    if (!w)
        return;
    if (w->unacked > 0)
        w->unacked--;
    w->success_counter++;
    if (w->success_counter >= w->window_size && w->window_size < 8) {
        w->window_size++;
        w->success_counter = 0;
    }
}

void flow_on_failure(flow_control_t* fc, uint32_t dest_addr) {
    flow_window_t* w = flow_find_or_create(fc, dest_addr);
    if (!w)
        return;
    w->window_size = w->window_size / 2;
    if (w->window_size < 1)
        w->window_size = 1;
    w->success_counter = 0;
}
