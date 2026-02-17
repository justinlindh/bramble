#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdbool.h>
#include <stdint.h>

#define MAILBOX_MAX_ENTRIES    32
#define MAILBOX_MAX_PER_DEST   8
#define MAILBOX_MAX_PER_SOURCE 8
#define MAILBOX_TTL_MS         (3600000UL * 24)  /* 24 hours */
#define MAILBOX_MAX_PAYLOAD    200

typedef struct {
    bool     active;
    uint32_t src_addr;
    uint32_t dest_addr;
    uint8_t  payload[MAILBOX_MAX_PAYLOAD];
    uint16_t payload_len;
    uint32_t stored_at_ms;
    uint32_t packet_id;
} mailbox_entry_t;

typedef struct {
    mailbox_entry_t entries[MAILBOX_MAX_ENTRIES];
    int  count;
    bool enabled;
} mailbox_t;

void mailbox_init(mailbox_t *mb);
int  mailbox_store(mailbox_t *mb, uint32_t src_addr, uint32_t dest_addr,
                   const uint8_t *payload, uint16_t len,
                   uint32_t packet_id, uint32_t now_ms);
int  mailbox_retrieve(mailbox_t *mb, uint32_t dest_addr,
                      mailbox_entry_t *out, int max_out);
void mailbox_purge_expired(mailbox_t *mb, uint32_t now_ms);
int  mailbox_count_for_dest(const mailbox_t *mb, uint32_t dest_addr);
int  mailbox_count_for_source(const mailbox_t *mb, uint32_t src_addr);

#endif /* MAILBOX_H */
