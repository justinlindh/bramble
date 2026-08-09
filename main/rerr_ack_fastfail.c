#include "rerr_ack_fastfail.h"

#include <inttypes.h>

#include "esp_log.h"
#include "msg_store.h"

static const char* TAG = "rerr_ack_fastfail";

size_t rerr_ack_failfast_for_dest(pending_ack_table_t* table, uint32_t broken_dest,
                                  const char* reason, rerr_ack_fail_notify_fn notify) {
    if (!table) {
        return 0;
    }

    size_t failed = 0;
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        pending_ack_t* pa = &table->entries[i];
        if (!pa->active || pa->dest_addr != broken_dest) {
            continue;
        }

        ESP_LOGW(TAG, "Fast-failing pkt %08" PRIX32 " to %08" PRIX32 " (reason=%s)", pa->packet_id,
                 pa->dest_addr, reason ? reason : "unknown");

        msg_store_update_status(pa->packet_id, MSG_STATUS_FAILED);
        if (notify) {
            notify(pa->packet_id, reason);
        }

        pa->active = false;
        failed++;
    }

    return failed;
}
