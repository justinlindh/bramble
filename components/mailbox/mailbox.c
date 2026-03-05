#include "mailbox.h"
#include <string.h>

void mailbox_init(mailbox_t* mb) {
    memset(mb, 0, sizeof(*mb));
    mb->enabled = true;
}

/* Find index of oldest active entry matching a filter, or -1. */
static int find_oldest(const mailbox_t* mb, uint32_t addr, bool by_dest) {
    int oldest = -1;
    uint32_t oldest_ts = UINT32_MAX;
    for (int i = 0; i < MAILBOX_MAX_ENTRIES; i++) {
        const mailbox_entry_t* e = &mb->entries[i];
        if (!e->active)
            continue;
        bool match = by_dest ? (e->dest_addr == addr) : (e->src_addr == addr);
        if (match && e->stored_at_ms < oldest_ts) {
            oldest_ts = e->stored_at_ms;
            oldest = i;
        }
    }
    return oldest;
}

static int find_oldest_global(const mailbox_t* mb) {
    int oldest = -1;
    uint32_t oldest_ts = UINT32_MAX;
    for (int i = 0; i < MAILBOX_MAX_ENTRIES; i++) {
        if (!mb->entries[i].active)
            continue;
        if (mb->entries[i].stored_at_ms < oldest_ts) {
            oldest_ts = mb->entries[i].stored_at_ms;
            oldest = i;
        }
    }
    return oldest;
}

static int find_free_slot(const mailbox_t* mb) {
    for (int i = 0; i < MAILBOX_MAX_ENTRIES; i++) {
        if (!mb->entries[i].active)
            return i;
    }
    return -1;
}

static bool has_duplicate(const mailbox_t* mb, uint32_t packet_id) {
    for (int i = 0; i < MAILBOX_MAX_ENTRIES; i++) {
        if (mb->entries[i].active && mb->entries[i].packet_id == packet_id)
            return true;
    }
    return false;
}

int mailbox_store(mailbox_t* mb, uint32_t src_addr, uint32_t dest_addr, const uint8_t* payload,
                  uint16_t len, uint32_t packet_id, uint32_t now_ms) {
    if (!mb || !payload || len == 0 || len > MAILBOX_MAX_PAYLOAD)
        return -1;

    /* Reject duplicate packet_id */
    if (has_duplicate(mb, packet_id))
        return -2;

    /* Evict per-dest if at cap */
    if (mailbox_count_for_dest(mb, dest_addr) >= MAILBOX_MAX_PER_DEST) {
        int idx = find_oldest(mb, dest_addr, true);
        if (idx >= 0) {
            mb->entries[idx].active = false;
            mb->count--;
        }
    }

    /* Evict per-source if at cap */
    if (mailbox_count_for_source(mb, src_addr) >= MAILBOX_MAX_PER_SOURCE) {
        int idx = find_oldest(mb, src_addr, false);
        if (idx >= 0) {
            mb->entries[idx].active = false;
            mb->count--;
        }
    }

    /* Find slot, evict globally if full */
    int slot = find_free_slot(mb);
    if (slot < 0) {
        slot = find_oldest_global(mb);
        if (slot < 0)
            return -1;
        mb->entries[slot].active = false;
        mb->count--;
    }

    mailbox_entry_t* e = &mb->entries[slot];
    e->active = true;
    e->src_addr = src_addr;
    e->dest_addr = dest_addr;
    memcpy(e->payload, payload, len);
    e->payload_len = len;
    e->stored_at_ms = now_ms;
    e->packet_id = packet_id;
    mb->count++;
    return 0;
}

int mailbox_retrieve(mailbox_t* mb, uint32_t dest_addr, mailbox_entry_t* out, int max_out) {
    int found = 0;
    for (int i = 0; i < MAILBOX_MAX_ENTRIES && found < max_out; i++) {
        mailbox_entry_t* e = &mb->entries[i];
        if (e->active && e->dest_addr == dest_addr) {
            out[found++] = *e;
            e->active = false;
            mb->count--;
        }
    }
    return found;
}

void mailbox_purge_expired(mailbox_t* mb, uint32_t now_ms) {
    for (int i = 0; i < MAILBOX_MAX_ENTRIES; i++) {
        mailbox_entry_t* e = &mb->entries[i];
        if (e->active && (now_ms - e->stored_at_ms) >= MAILBOX_TTL_MS) {
            e->active = false;
            mb->count--;
        }
    }
}

int mailbox_count_for_dest(const mailbox_t* mb, uint32_t dest_addr) {
    int c = 0;
    for (int i = 0; i < MAILBOX_MAX_ENTRIES; i++)
        if (mb->entries[i].active && mb->entries[i].dest_addr == dest_addr)
            c++;
    return c;
}

int mailbox_count_for_source(const mailbox_t* mb, uint32_t src_addr) {
    int c = 0;
    for (int i = 0; i < MAILBOX_MAX_ENTRIES; i++)
        if (mb->entries[i].active && mb->entries[i].src_addr == src_addr)
            c++;
    return c;
}
