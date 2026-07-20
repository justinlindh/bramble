/**
 * mesh_mailbox.c: Store-and-forward mailbox glue over components/mailbox.
 *
 * Split out of mesh_task.c (issue #86); pure code motion, no behavior change.
 * Shared state and cross-module entry points come from mesh_internal.h.
 */
#include "mesh_internal.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_keys.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"

static const char* TAG = "mesh";

/* ── Data forwarding for multi-hop ──────────────────────────────────── */

/* ── Mailbox helpers ─────────────────────────────────────────────────── */

bool mesh_mailbox_store(uint32_t src_addr, uint32_t dest_addr, const uint8_t* raw,
                               uint8_t raw_len, uint32_t packet_id) {
    if (!s_mailbox_enabled)
        return false;

    /* Component payload is capped at MAILBOX_MAX_PAYLOAD (200) bytes.
     * Raw packets that exceed this limit cannot be buffered; drop with a warning. */
    if (raw_len > MAILBOX_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Mailbox: packet too large to store (%u > %u bytes), dropping for %08" PRIX32,
                 raw_len, (unsigned)MAILBOX_MAX_PAYLOAD, dest_addr);
        return false;
    }

    int rc = mailbox_store(&s_mailbox, src_addr, dest_addr, raw, raw_len, packet_id, now_ms());
    if (rc == 0) {
        ESP_LOGI(TAG, "Mailbox: stored packet for %08" PRIX32 " (id=%08" PRIX32 ")", dest_addr,
                 packet_id);
        return true;
    } else if (rc == -2) {
        ESP_LOGD(TAG, "Mailbox: duplicate packet id=%08" PRIX32 ", not stored", packet_id);
    } else {
        ESP_LOGW(TAG, "Mailbox: store failed (rc=%d) for %08" PRIX32, rc, dest_addr);
    }
    return false;
}

void mailbox_flush_for(uint32_t dest_addr) {
    mailbox_entry_t entries[MAILBOX_MAX_PER_DEST];
    int count = mailbox_retrieve(&s_mailbox, dest_addr, entries, MAILBOX_MAX_PER_DEST);
    for (int i = 0; i < count; i++) {
        /* Wire v4: mailbox entries are raw DATA packet bytes captured at
         * store time (mesh_mailbox_store, forward_data_packet's no-route
         * branch), so their prev_hop byte range reflects whoever wrote it
         * back then, not us. WE are the transmitter on this flush, so
         * rewrite prev_hop to our own address before TX, exactly like
         * forward_data_packet's rewrite -- otherwise the recipient would
         * learn a stale/wrong reverse-route hop from a store-and-forward
         * delivery. */
        if (entries[i].payload_len >= BRAMBLE_DATA_PREV_HOP_OFFSET + 4) {
            memcpy(entries[i].payload + BRAMBLE_DATA_PREV_HOP_OFFSET, &s_identity->address, 4);
        }
        ESP_LOGI(TAG,
                 "Mailbox: delivering stored packet to %08" PRIX32 " (id=%08" PRIX32 " len=%u)",
                 dest_addr, entries[i].packet_id, entries[i].payload_len);
        /* Deny behavior: budget denial and radio failure both re-store the
         * entry for the next flush; stored mail is never silently lost. */
        int rc = mesh_tx(entries[i].payload, (uint8_t)entries[i].payload_len, TX_KIND_MAILBOX);
        if (rc != 0) {
            /* Transmit failed (LBT / radio busy), re-store for retry on next flush */
            ESP_LOGW(TAG, "Mailbox: transmit failed (rc=%d) for id=%08" PRIX32 ", re-queuing", rc,
                     entries[i].packet_id);
            mailbox_store(&s_mailbox, entries[i].src_addr, entries[i].dest_addr, entries[i].payload,
                          entries[i].payload_len, entries[i].packet_id, entries[i].stored_at_ms);
        }
    }
}

void mailbox_expire(uint32_t t) { mailbox_purge_expired(&s_mailbox, t); }

void mesh_set_mailbox(bool enabled) {
    s_mailbox_enabled = enabled;
    ESP_LOGI(TAG, "Mailbox runtime: %s", enabled ? "enabled" : "disabled");
}

bool mesh_get_mailbox(void) { return s_mailbox_enabled; }
