#ifndef BRAMBLE_RELIABILITY_H
#define BRAMBLE_RELIABILITY_H
#include <stdint.h>
#include <stdbool.h>

#define MSG_TIER_BROADCAST 0
#define MSG_TIER_NORMAL 1
#define MSG_TIER_CRITICAL 2

#define MAX_PENDING_ACKS 8

/*
 * A retransmit re-sends the byte-exact original packet, so this buffer must
 * hold the largest packet that can be registered for ACK. The transmit path
 * caps an on-air packet at 255 bytes (mesh_tx takes a uint8_t length), which
 * is BRAMBLE_MAX_PACKET_SIZE in components/packet. reliability is
 * intentionally free of cross-component dependencies, so the bound is
 * restated here rather than included; keep it in sync if the packet cap
 * grows. The previous 222 was smaller than a full DATA packet, so a large
 * unicast send was truncated on copy while packet_len still recorded the
 * full length, over-reading past this buffer on retransmit.
 */
#define PENDING_ACK_PACKET_CAP 256

typedef struct {
    uint32_t packet_id;
    uint32_t dest_addr;
    uint8_t tier;
    uint8_t attempt;
    uint8_t max_attempts;
    uint32_t next_retry_ms;
    uint16_t packet_len;
    uint8_t packet_data[PENDING_ACK_PACKET_CAP];
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
/*
 * Task 6 (GAP B): the single source of truth for which reliability tier a
 * DATA send registers under. Key exchange (handshake-in-DATA, APP_TYPE_KE)
 * must use MSG_TIER_CRITICAL (8 retries) per spec: losing a handshake
 * message stalls session establishment entirely, unlike an ordinary chat
 * send. Takes a plain bool rather than the channel component's APP_TYPE_KE
 * constant so components/reliability has no dependency on components/
 * channel; the caller (mesh_task.c's send_data_packet) passes
 * `app_type == APP_TYPE_KE`. This is the ONLY place tier-for-send is
 * decided, so a caller cannot silently regress back to MSG_TIER_NORMAL for
 * KE without this function (and its test) catching it.
 */
uint8_t msg_tier_for_send(bool is_key_exchange);
#endif
