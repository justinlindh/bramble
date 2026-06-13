#ifndef BRAMBLE_RELIABILITY_H
#define BRAMBLE_RELIABILITY_H
#include <stdint.h>
#include <stdbool.h>

#define MSG_TIER_BROADCAST 0
#define MSG_TIER_NORMAL 1
#define MSG_TIER_CRITICAL 2

#define MAX_PENDING_ACKS 8

typedef struct {
    uint32_t packet_id;
    uint32_t dest_addr;
    uint8_t tier;
    uint8_t attempt;
    uint8_t max_attempts;
    uint32_t next_retry_ms;
    uint16_t packet_len;
    uint8_t packet_data[222];
    bool active;
} pending_ack_t;

typedef struct {
    pending_ack_t entries[MAX_PENDING_ACKS];
} pending_ack_table_t;

void pending_ack_init(pending_ack_table_t* table);
int pending_ack_add(pending_ack_table_t* table, uint32_t packet_id, uint32_t dest_addr,
                    uint8_t tier, const uint8_t* packet, uint16_t len, uint32_t now_ms);
bool pending_ack_remove(pending_ack_table_t* table, uint32_t packet_id);
void pending_ack_tick(pending_ack_table_t* table, uint32_t now_ms);
uint8_t tier_max_retries(uint8_t tier);
uint32_t tier_base_delay_ms(uint8_t tier);
#endif
