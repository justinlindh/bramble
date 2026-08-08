#ifndef BRAMBLE_RELIABILITY_H
#define BRAMBLE_RELIABILITY_H
#include <stdint.h>
#include <stdbool.h>

#define MSG_TIER_BROADCAST 0
#define MSG_TIER_NORMAL 1
#define MSG_TIER_CRITICAL 2

#define MAX_PENDING_ACKS 8

/* A retransmittable frame is the full on-air DATA frame, which the sender
 * caps at 255 bytes (the uint8_t PHY length limit). The stored copy must be
 * able to hold a maximum-size frame verbatim, or a retry would transmit a
 * truncated (and thus auth/decrypt-failing) frame. */
#define PENDING_ACK_MAX_FRAME 255

typedef struct {
    uint32_t packet_id;
    uint32_t dest_addr;
    uint8_t tier;
    uint8_t attempt;
    uint8_t max_attempts;
    uint32_t next_retry_ms;
    uint16_t packet_len;
    uint8_t packet_data[PENDING_ACK_MAX_FRAME];
    bool active;
} pending_ack_t;

typedef struct {
    pending_ack_t entries[MAX_PENDING_ACKS];
} pending_ack_table_t;

void pending_ack_init(pending_ack_table_t* table);
int pending_ack_add(pending_ack_table_t* table, uint32_t packet_id, uint32_t dest_addr,
                    uint8_t tier, const uint8_t* packet, uint16_t len, uint32_t now_ms);
bool pending_ack_remove(pending_ack_table_t* table, uint32_t packet_id);

/*
 * Is this frame still outstanding: transmitted, and neither acknowledged nor
 * given up on. packet_id 0 is never outstanding, since it is the value a row
 * carries when no frame has gone out for it at all.
 *
 * Exists for the parked-message retry, which must not put one written message
 * on the air twice. The send queue refuses a uid it already holds, but that
 * only covers a message still WAITING for a route or a session; once the frame
 * has been transmitted it holds no queue entry, and this table is the only
 * thing that knows the attempt is unresolved.
 */
bool pending_ack_is_active(const pending_ack_table_t* table, uint32_t packet_id);
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
