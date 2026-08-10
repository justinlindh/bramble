/**
 * mesh_reliability.c: ACKs, delivery receipts, the delivery-event ring, and broadcast telemetry.
 *
 * Split out of mesh_task.c (issue #86); pure code motion, no behavior change.
 * Shared state and cross-module entry points come from mesh_internal.h.
 */
#include "mesh_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"

#ifdef CONFIG_BRAMBLE_UI_GRAPHICAL
#include "ui_graphics.h"
#endif

static const char* TAG = "mesh";

enum {
    DELIVERY_EVENT_TYPE_ACK = 1,
    DELIVERY_EVENT_TYPE_BROADCAST_DELIVERY = 2,
};

/* Forward declarations for intra-module static helpers. */
static void delivery_event_ring_append_locked(const delivery_event_record_t* event);
static void record_ack_delivery_event(const bramble_ack_t* ack);
static void record_broadcast_delivery_event(uint32_t recipient_addr, uint32_t broadcast_id,
                                            uint8_t hop_count, const uint32_t* relay_path);
static bool recent_broadcast_contains(uint32_t packet_id);
static void mesh_schedule_next_receipt_timer(void);
static void forward_ack(bramble_ack_t* ack);
static void forward_delivery_receipt(bramble_delivery_receipt_t* receipt);

static void delivery_event_ring_append_locked(const delivery_event_record_t* event) {
    if (!event || !s_delivery_event_mutex || !s_delivery_event_ring)
        return;
    xSemaphoreTake(s_delivery_event_mutex, portMAX_DELAY);
    delivery_event_ring_append(s_delivery_event_ring, event);
    xSemaphoreGive(s_delivery_event_mutex);
}

static void record_ack_delivery_event(const bramble_ack_t* ack) {
    if (!ack)
        return;

    delivery_event_record_t evt = {0};
    evt.message_id = ack->ack_packet_id;
    evt.timestamp_s = now_ms() / 1000u;
    evt.recipient_addr = ack->src_addr;
    evt.source_addr = s_identity ? s_identity->address : 0u;
    evt.event_type = DELIVERY_EVENT_TYPE_ACK;
    evt.tier = MSG_TIER_NORMAL;

    uint8_t hops = ack->hop_count;
    if (hops > DELIVERY_EVENT_ROUTE_MAX_HOPS)
        hops = DELIVERY_EVENT_ROUTE_MAX_HOPS;
    evt.route_len = hops;
    for (uint8_t i = 0; i < hops; i++) {
        evt.route_hops[i] = ack->relay_path[i];
    }

    delivery_event_ring_append_locked(&evt);
}

static void record_broadcast_delivery_event(uint32_t recipient_addr, uint32_t broadcast_id,
                                            uint8_t hop_count, const uint32_t* relay_path) {
    delivery_event_record_t evt = {0};
    evt.message_id = broadcast_id;
    evt.timestamp_s = now_ms() / 1000u;
    evt.recipient_addr = recipient_addr;
    evt.source_addr = s_identity ? s_identity->address : 0u;
    evt.event_type = DELIVERY_EVENT_TYPE_BROADCAST_DELIVERY;
    evt.tier = MSG_TIER_BROADCAST;

    uint8_t hops = hop_count;
    if (hops > DELIVERY_EVENT_ROUTE_MAX_HOPS)
        hops = DELIVERY_EVENT_ROUTE_MAX_HOPS;
    evt.route_len = hops;
    if (relay_path) {
        for (uint8_t i = 0; i < hops; i++) {
            evt.route_hops[i] = relay_path[i];
        }
    }

    delivery_event_ring_append_locked(&evt);
}

void recent_broadcast_record(uint32_t packet_id) {
    if (packet_id == 0)
        return;
    s_recent_broadcast_ids[s_recent_broadcast_idx] = packet_id;
    s_recent_broadcast_idx = (s_recent_broadcast_idx + 1) % RECENT_BROADCAST_RING_SIZE;
}

static bool recent_broadcast_contains(uint32_t packet_id) {
    if (packet_id == 0)
        return false;
    for (int i = 0; i < RECENT_BROADCAST_RING_SIZE; i++) {
        if (s_recent_broadcast_ids[i] == packet_id) {
            return true;
        }
    }
    return false;
}

void maybe_emit_implicit_broadcast_delivery(const bramble_header_t* header,
                                            const rx_packet_t* pkt) {
    if (!header || !pkt)
        return;
    if (header->type != PKT_TYPE_DATA)
        return;
    if (header->dest_addr != 0xFFFFFFFFu)
        return;
    if (!recent_broadcast_contains(header->packet_id))
        return;
    if (pkt->len < HEADER_SIZE + sizeof(uint32_t))
        return;

    uint32_t relayer_addr = 0;
    memcpy(&relayer_addr, pkt->data + HEADER_SIZE, sizeof(relayer_addr));
    if (relayer_addr == 0)
        return;

    uint32_t relay_path[1] = {relayer_addr};
    mesh_emit_broadcast_delivery_notification(relayer_addr, header->packet_id, 0, 1, relay_path);
    record_broadcast_delivery_event(relayer_addr, header->packet_id, 1, relay_path);
}

/* ── ACK handling ────────────────────────────────────────────────────── */

static void mesh_schedule_next_receipt_timer(void) {
    if (!s_receipt_timer)
        return;

    uint32_t t = now_ms();
    uint32_t earliest_due = 0;
    bool have_pending = false;

    for (int i = 0; i < RECEIPT_QUEUE_CAPACITY; i++) {
        if (!s_receipt_queue[i].used)
            continue;
        if (!have_pending || s_receipt_queue[i].due_at_ms < earliest_due) {
            earliest_due = s_receipt_queue[i].due_at_ms;
            have_pending = true;
        }
    }

    if (!have_pending) {
        esp_timer_stop(s_receipt_timer);
        return;
    }

    uint32_t delay_ms = (earliest_due <= t) ? 1u : (earliest_due - t);
    esp_timer_stop(s_receipt_timer);
    esp_err_t err = esp_timer_start_once(s_receipt_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to arm receipt timer: %d", (int)err);
    }
}

void queue_broadcast_delivery_receipt(uint32_t original_src_addr, uint32_t original_packet_id) {
    /* Mandatory-provisioning (Task 2): inert when unprovisioned (the receipt is
     * receipt_sign'd with the network key inside the builder). */
    if (!network_key_is_provisioned()) {
        ESP_LOGD(TAG, "unprovisioned: inert, skipping delivery receipt");
        return;
    }

    uint8_t buf[DELIVERY_RECEIPT_MAX_SIZE];
    size_t wire_len = 0;

    /* Determine receipt policy based on mesh size */
    uint8_t policy =
        mesh_broadcast_receipt_policy(0xFFFFFFFFu, (uint8_t)neighbor_count(&s_neighbors));
    uint8_t hop_limit = (policy >= 2) ? 8 : 1; /* full=8, neighbors-only=1 */

    /* ws 1.3b: draw the 48-bit origin seq once per receipt; the retry
     * queue below resends the SAME serialized bytes on loss (not a fresh
     * re-origination), so one seq draw per receipt is correct. Fail-closed:
     * no seq means no receipt goes out this round. */
    uint64_t receipt_seq;
    if (control_seq_next(&receipt_seq) != 0) {
        ESP_LOGE(TAG, "Seq counter unavailable, dropping delivery receipt for pkt=%08" PRIX32,
                 original_packet_id);
        return;
    }

    esp_err_t err = mesh_build_broadcast_delivery_receipt_packet(
        s_identity->address, next_packet_id(), original_src_addr, original_packet_id, hop_limit,
        receipt_seq, buf, sizeof(buf), &wire_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Delivery receipt build failed: %d", (int)err);
        return;
    }

    int slot = -1;
    for (int i = 0; i < RECEIPT_QUEUE_CAPACITY; i++) {
        if (!s_receipt_queue[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "Delivery receipt queue full; dropping pkt=%08" PRIX32, original_packet_id);
        return;
    }

    uint32_t slot_delay_ms = mesh_broadcast_receipt_slot_delay_ms(
        s_identity->address, original_packet_id, (uint8_t)neighbor_count(&s_neighbors));
    uint32_t initial_delay_ms = slot_delay_ms + (esp_random() % 400u); /* +0..399ms jitter */

    pending_receipt_t* item = &s_receipt_queue[slot];
    memset(item, 0, sizeof(*item));
    item->used = true;
    item->original_src_addr = original_src_addr;
    item->original_packet_id = original_packet_id;
    memcpy(item->buf, buf, wire_len);
    item->wire_len = (uint8_t)wire_len;
    item->attempts_total = mesh_broadcast_receipt_retry_count();
    if (item->attempts_total == 0) {
        item->attempts_total = 1;
    }
    item->attempts_sent = 0;
    item->due_at_ms = now_ms() + initial_delay_ms;

    mesh_schedule_next_receipt_timer();
}

/* Airtime-pressure multiplier for delivery-receipt retry delays. Both the
 * deny-backoff and the normal-retry path scale their timings by the same
 * factor derived from the remaining receipt budget; this reads the budget
 * once, resolves the num/den scale, and logs it when it is not unity. */
static void receipt_retry_scale(uint32_t* scale_num, uint32_t* scale_den) {
    uint32_t remaining = tx_gate_remaining(AIRTIME_TIER_RECEIPT);
    *scale_num = 1u;
    *scale_den = 1u;
    mesh_broadcast_receipt_retry_scale(remaining, scale_num, scale_den);
    if (!(*scale_num == 1u && *scale_den == 1u)) {
        uint32_t utilized_pct =
            ((AIRTIME_BUDGET_RECEIPT_MS -
              (remaining > AIRTIME_BUDGET_RECEIPT_MS ? AIRTIME_BUDGET_RECEIPT_MS : remaining)) *
             100u) /
            AIRTIME_BUDGET_RECEIPT_MS;
        ESP_LOGD(TAG,
                 "Receipt retry multiplier=%" PRIu32 "/%" PRIu32 " (utilization=%" PRIu32 "%%)",
                 *scale_num, *scale_den, utilized_pct);
    }
}

void mesh_process_receipt_tx_event(void) {
    uint32_t t_now = now_ms();
    int due_idx = -1;

    for (int i = 0; i < RECEIPT_QUEUE_CAPACITY; i++) {
        if (!s_receipt_queue[i].used)
            continue;
        if (s_receipt_queue[i].due_at_ms <= t_now) {
            due_idx = i;
            break;
        }
    }

    if (due_idx < 0) {
        mesh_schedule_next_receipt_timer();
        return;
    }

    pending_receipt_t* item = &s_receipt_queue[due_idx];
    uint8_t attempt_no = (uint8_t)(item->attempts_sent + 1u);

    /* TX path can block for CAD/LBT + radio wait; feed task WDT just before entering it. */
    esp_task_wdt_reset();

    int rc = mesh_tx(item->buf, item->wire_len, TX_KIND_RECEIPT);

    /* Deny behavior: receipts are deferred, not dropped. Reschedule with
     * exponential backoff (scaled by remaining receipt budget) so the
     * receipt can go out once tokens refill; drop only when all attempts
     * are exhausted. */
    if (rc == TX_GATE_ERR_BUDGET) {
        item->attempts_sent++;
        /* This spends the attempt, so the consecutive channel-busy defer
         * count starts over with the next one (the documented invariant on
         * pending_receipt_t.defers). Reachable when a budget deny
         * interleaves with channel-busy defers on the same attempt. */
        item->defers = 0;
        if (item->attempts_sent >= item->attempts_total) {
            ESP_LOGW(TAG,
                     "Delivery receipt DROPPED for pkt=%08" PRIX32
                     " (all %u attempts airtime-exhausted)",
                     item->original_packet_id, (unsigned)item->attempts_total);
            memset(item, 0, sizeof(*item));
        } else {
            uint32_t scale_num = 1u;
            uint32_t scale_den = 1u;
            receipt_retry_scale(&scale_num, &scale_den);
            uint32_t raw_backoff_ms =
                1000u + ((uint32_t)item->attempts_sent * 2000u) + (esp_random() % 1000u);
            uint32_t backoff_ms = (raw_backoff_ms * scale_num) / scale_den;
            item->due_at_ms = t_now + backoff_ms;
            ESP_LOGW(TAG,
                     "Delivery receipt deferred for pkt=%08" PRIX32
                     " (attempt=%u/%u): airtime exhausted, retry in %" PRIu32 "ms",
                     item->original_packet_id, (unsigned)(item->attempts_sent),
                     (unsigned)item->attempts_total, backoff_ms);
        }
        mesh_schedule_next_receipt_timer();
        return;
    }

    /* Channel-busy deferral: LBT never found a quiet channel, so nothing
     * went out and nothing was debited. Blind-firing here is exactly what
     * lost 20-25% of broadcast receipts on the bench (every node answers
     * the same origin at once, so a busy channel means another receipt is
     * in flight), so re-run the SAME attempt after a short jittered wait
     * rather than spending it. Bounded by RECEIPT_MAX_DEFERS so a
     * permanently jammed channel still terminates: at the cap the attempt
     * counts as spent and the receipt rejoins the normal retry/exhaustion
     * path, which is what a blind-fired-and-lost attempt did before. */
    if (rc == TX_GATE_ERR_CHANNEL_BUSY) {
        /* Re-read the clock: t_now predates mesh_tx, whose LBT loop blocks
         * through up to three real backoff delays before giving up. Anchoring
         * the defer on the stale value would silently spend most of the
         * 250-999ms window inside the call that just failed, re-firing into
         * the same busy channel the defer exists to avoid. */
        uint32_t t_after_lbt = now_ms();
        item->defers++;
        if (item->defers < RECEIPT_MAX_DEFERS) {
            uint32_t defer_delay_ms = 250u + (esp_random() % 750u);
            item->due_at_ms = t_after_lbt + defer_delay_ms;
            ESP_LOGD(TAG,
                     "Delivery receipt deferred for pkt=%08" PRIX32
                     " (attempt=%u/%u defer=%u/%u): channel busy, retry in %" PRIu32 "ms",
                     item->original_packet_id, (unsigned)attempt_no, (unsigned)item->attempts_total,
                     (unsigned)item->defers, (unsigned)RECEIPT_MAX_DEFERS, defer_delay_ms);
            mesh_schedule_next_receipt_timer();
            return;
        }

        item->defers = 0;
        item->attempts_sent++;
        if (item->attempts_sent >= item->attempts_total) {
            ESP_LOGW(TAG,
                     "Delivery receipt DROPPED for pkt=%08" PRIX32
                     " (all %u attempts channel-blocked)",
                     item->original_packet_id, (unsigned)item->attempts_total);
            memset(item, 0, sizeof(*item));
        } else {
            uint32_t scale_num = 1u;
            uint32_t scale_den = 1u;
            receipt_retry_scale(&scale_num, &scale_den);
            uint32_t raw_backoff_ms =
                1000u + ((uint32_t)item->attempts_sent * 2000u) + (esp_random() % 1000u);
            uint32_t backoff_ms = (raw_backoff_ms * scale_num) / scale_den;
            item->due_at_ms = t_after_lbt + backoff_ms;
            ESP_LOGW(TAG,
                     "Delivery receipt deferred for pkt=%08" PRIX32
                     " (attempt=%u/%u): channel busy for %u defers, retry in %" PRIu32 "ms",
                     item->original_packet_id, (unsigned)(item->attempts_sent),
                     (unsigned)item->attempts_total, (unsigned)RECEIPT_MAX_DEFERS, backoff_ms);
        }
        mesh_schedule_next_receipt_timer();
        return;
    }

    if (rc == TX_GATE_OK) {
        ESP_LOGI(TAG,
                 "TX delivery receipt for broadcast pkt=%08" PRIX32 " to %08" PRIX32
                 " attempt=%u/%u",
                 item->original_packet_id, item->original_src_addr, (unsigned)attempt_no,
                 (unsigned)item->attempts_total);
    }

    item->attempts_sent++;
    item->defers = 0; /* the attempt was spent on the air; defers are per-attempt */
    if (item->attempts_sent >= item->attempts_total) {
        memset(item, 0, sizeof(*item));
        mesh_schedule_next_receipt_timer();
        return;
    }

    uint32_t scale_num = 1u;
    uint32_t scale_den = 1u;
    receipt_retry_scale(&scale_num, &scale_den);

    /* Retry spacing re-draws a FULL contention slot, salted by the attempt
     * number, instead of the old short fixed backoff (500+700i ms plus small
     * jitter). Bench telemetry showed why the old shape lost receipts: first
     * attempts are slot-spread across a window sized to the peer count, but
     * short-backoff retries folded attempts 2 and 3 back into OTHER nodes'
     * first-attempt slots, so during a 9-receipt storm each transmission was
     * received by fewer than half the nodes in range and some receipts lost
     * every copy at the origin. Salting the slot hash with the attempt
     * number scatters each retry into a fresh pseudo-random slot of the
     * next window, decorrelated from every other sender's attempts, at
     * unchanged TX volume. The budget-pressure scale still stretches the
     * result when the receipt lane is under pressure. */
    uint32_t reslot_ms = mesh_broadcast_receipt_slot_delay_ms(
        s_identity->address, item->original_packet_id ^ ((uint32_t)item->attempts_sent << 28),
        (uint8_t)neighbor_count(&s_neighbors));
    uint32_t retry_delay_ms = ((reslot_ms + (esp_random() % 400u)) * scale_num) / scale_den;
    if (retry_delay_ms == 0u) {
        retry_delay_ms = 1u;
    }
    item->due_at_ms = now_ms() + retry_delay_ms;

    mesh_schedule_next_receipt_timer();
}

void mesh_receipt_timer_cb(void* arg) {
    (void)arg;
    if (!s_mesh_event_queue)
        return;

    mesh_event_type_t evt = MESH_EVT_RECEIPT_TX;
    if (xQueueSend(s_mesh_event_queue, &evt, 0) != pdTRUE) {
        ESP_EARLY_LOGW(TAG, "mesh event queue full; dropped receipt timer event");
    }
}

void send_ack(uint32_t dest_addr, uint32_t ack_packet_id, int8_t rssi) {
    /* Mandatory-provisioning (Task 2): inert when unprovisioned (ack_sign
     * needs the network key). The sender's retransmission timer covers the
     * missing ACK exactly like a lost one. */
    if (!network_key_is_provisioned()) {
        ESP_LOGD(TAG, "unprovisioned: inert, skipping ACK");
        return;
    }
    /* ws 1.3b: draw the 48-bit origin seq before building the struct, so it
     * can go straight into the designated initializer below instead of a
     * second pass. Fail-closed: no seq means no ACK goes out; the sender's
     * retransmission timer covers a missing ACK exactly like a lost one. */
    uint64_t ack_seq;
    if (control_seq_next(&ack_seq) != 0) {
        ESP_LOGE(TAG, "Seq counter unavailable, dropping ACK for pkt=%08" PRIX32, ack_packet_id);
        return;
    }
    bramble_ack_t ack = {
        .header =
            {
                .version = BRAMBLE_VERSION,
                .type = PKT_TYPE_ACK,
                .flags = 0,
                /* Reactive: ROUTE_HOP_LIMIT_MAX (8). Flood transport: the
                 * flooded-ACK originates at the operator-settable flood hop
                 * budget so a confirmation can traverse the same diameter its
                 * DATA did. */
                .hop_limit = flood_origination_hop_limit(s_flood_transport, s_flood_hop_limit),
                .dest_addr = dest_addr,
                .packet_id = next_packet_id(),
            },
        .src_addr = s_identity->address,
        .ack_packet_id = ack_packet_id,
        .ack_flags = 0,
        .rssi_at_dest = rssi,
        .hop_count = 1,
        .relay_path = {s_identity->address}, /* destination is first hop */
    };
    bramble_seq48_pack(ack.seq, ack_seq);
    /* NEW-SEC-8 (STAGED): sign after every field except relay_path/
     * hop_count/hop_limit is set (those are excluded from the MAC and
     * legitimately change per relay hop); seq is set above and IS covered
     * (ws 1.3b). */
    ack_sign(&ack);

    /* Red-team audit: was buf[64], a hand-counted constant. Not currently
     * truncating (send_ack always originates with hop_count 1), but
     * macro-ized to ACK_MAX_SIZE anyway so it can't silently start
     * truncating if that ever changes, matching forward_ack's fix below. */
    uint8_t buf[ACK_MAX_SIZE];
    esp_err_t err = bramble_ack_serialize(&ack, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ACK serialize failed");
        return;
    }
    size_t wire_len = bramble_ack_wire_size(&ack);
    /* Deny behavior: nothing to queue; the sender's retry scheduler covers
     * a lost ACK. CRITICAL can borrow from NORMAL, so denial here means
     * the node is severely over budget. */
    int ret = mesh_tx(buf, (uint8_t)wire_len, TX_KIND_ACK);
    if (ret == TX_GATE_OK) {
        ESP_LOGI(TAG, "ACK sent for pkt %08" PRIX32 " to %08" PRIX32 " (%u hops)", ack_packet_id,
                 dest_addr, ack.hop_count);
    }
}

static void forward_ack(bramble_ack_t* ack) {
    /* Append our address to the relay path */
    if (ack->hop_count < ACK_MAX_HOPS) {
        ack->relay_path[ack->hop_count++] = s_identity->address;
    }

    /* Decrement hop limit */
    if (ack->header.hop_limit <= 1) {
        ESP_LOGD(TAG, "ACK hop limit reached, dropping");
        return;
    }
    ack->header.hop_limit--;

    /* Look up route back to the original sender */
    route_entry_t* route = route_lookup(&s_routes, ack->header.dest_addr);
    if (!route || route->state == ROUTE_BROKEN) {
        ESP_LOGW(TAG, "No route to forward ACK to %08" PRIX32, ack->header.dest_addr);
        return;
    }

    /* Red-team fix: was buf[64], a hand-counted constant. ACK_MAX_SIZE (a
     * full 8-hop path) is 69 as of the ws 1.3b size bump, so a 7-hop (65B)
     * or 8-hop (69B) ACK overflowed this buffer and was silently dropped
     * (bramble_ack_serialize's len < need guard). */
    uint8_t buf[ACK_MAX_SIZE];
    esp_err_t err = bramble_ack_serialize(ack, buf, sizeof(buf));
    if (err != ESP_OK)
        return;

    size_t wire_len = bramble_ack_wire_size(ack);
    ESP_LOGI(TAG, "Forwarding ACK for pkt %08" PRIX32 " toward %08" PRIX32 " (%u hops)",
             ack->ack_packet_id, ack->header.dest_addr, ack->hop_count);
    mesh_tx(buf, (uint8_t)wire_len, TX_KIND_ACK);
}

void handle_ack(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    bramble_ack_t ack;
    esp_err_t err = bramble_ack_deserialize(&ack, data, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ACK deserialize failed");
        return;
    }

    /* NEW-SEC-8 (STAGED, see network_key.h): verify before ANY effect of
     * this ACK, on both branches below. A forged ACK must not cancel
     * retransmission, mark a message delivered, or be forwarded. */
    if (!ack_verify(&ack)) {
        ESP_LOGW(TAG, "ACK auth failed pkt=%08" PRIX32 " src=%08" PRIX32, ack.ack_packet_id,
                 ack.src_addr);
        return;
    }

    /* ws 1.3b: replay check on the authenticated signer (ack.src_addr is
     * MAC-covered, so an attacker cannot dodge the window by mutating it).
     * Checked after ack_verify and strictly before BOTH the forward branch
     * and the for-us effects below (pending_ack_remove, msg_store_update),
     * so a replayed ACK never cancels a live retransmission or forwards. */
    uint64_t ack_seq = bramble_seq48_unpack(ack.seq);
    if (!control_replay_ok(ack.src_addr, ack_seq)) {
        ESP_LOGW(TAG, "ACK replay pkt=%08" PRIX32 " src=%08" PRIX32, ack.ack_packet_id,
                 ack.src_addr);
        return;
    }

    /* Liveness from traffic (see the DATA case in mesh_process_rx_packet).
     * hop_count 0 means no relay has appended itself yet, so the ACK's signer
     * is also its transmitter, and ack.src_addr is MAC-covered by the
     * ack_verify above. Placed before the forward branch on purpose: hearing a
     * neighbor's own ACK proves it is alive whether or not the ACK is for us. */
    if (ack.hop_count == 0) {
        mesh_note_peer_heard(ack.src_addr, rssi, snr);
    }

    /* Not for us, forward it */
    if (ack.header.dest_addr != s_identity->address) {
        if (s_flood_transport) {
            /* Flooding F1 Task 2: under s_flood_transport there are no routes,
             * so the ACK cannot be route-forwarded home. It FLOODS back
             * through the SAME engine the DATA flood uses (channel_flood_decide
             * + the jittered schedule_flood_relay queue + FLOOD_SUPPRESS_AFTER
             * suppression + airtime budget), authenticated: ack_verify above
             * already gated this branch, so a bad-MAC ACK was dropped before
             * ever reaching here and is never rebroadcast (the same "never act
             * on unauthenticated wire bytes" rule the DATA flood applies via
             * data_auth_verify). The dispatch s_dedup gate (packet_id ^ type)
             * already dedups the flooded ACK's own packet_id: copies 2+ never
             * reach handle_ack; they are counted at the dispatch gate for
             * suppression instead (see mesh_process_rx_packet). A re-ACK of a
             * duplicate DATA carries a FRESH header.packet_id (send_ack draws
             * next_packet_id every call), so it is not deduped and floods
             * anew, preserving the Phase 1 re-ACK-on-duplicate second chance.
             *
             * The flood dedup key mirrors the DATA flood's packet_id ^ src:
             * both fields are stable across relay hops (only relay_path/
             * hop_count/hop_limit are forward-mutated) and identify this ACK
             * for the suppression bookkeeping at the dispatch gate. A node that
             * hears its OWN originated ACK echoed back (ack.src_addr == self)
             * must not rebroadcast it, exactly like the DATA flood's
             * is_own_echo guard. */
            uint32_t ack_flood_key = ack.header.packet_id ^ ack.src_addr;
            bool is_own_echo = (ack.src_addr == s_identity->address);
            size_t cur_wire = bramble_ack_wire_size(&ack);
            bool budget_permits = tx_gate_check((uint8_t)cur_wire, TX_KIND_ACK);
            channel_flood_decision_t flood = channel_flood_decide(ack.header.hop_limit, is_own_echo,
                                                                  budget_permits, esp_random());
            if (flood.should_relay) {
                /* Append our address to the relay trail (relay_path/hop_count
                 * are MAC-excluded, mutated per hop exactly as forward_ack
                 * does) and decrement the hop limit to the flood engine's
                 * value, then re-serialize the mutated ACK for rebroadcast. */
                if (ack.hop_count < ACK_MAX_HOPS) {
                    ack.relay_path[ack.hop_count++] = s_identity->address;
                }
                ack.header.hop_limit = flood.new_hop_limit;
                uint8_t relay_buf[ACK_MAX_SIZE];
                if (bramble_ack_serialize(&ack, relay_buf, sizeof(relay_buf)) == ESP_OK) {
                    size_t wlen = bramble_ack_wire_size(&ack);
                    ESP_LOGI(TAG,
                             "Flooding ACK for pkt %08" PRIX32 " toward %08" PRIX32
                             " hop_limit->%u",
                             ack.ack_packet_id, ack.header.dest_addr, flood.new_hop_limit);
                    schedule_flood_relay(relay_buf, (uint8_t)wlen, flood.jitter_ms, ack_flood_key,
                                         TX_KIND_ACK);
                }
            } else if (!budget_permits) {
                ESP_LOGD(TAG, "Flooded ACK relay denied by airtime budget, pkt=%08" PRIX32,
                         ack.ack_packet_id);
            }
        } else {
            forward_ack(&ack);
        }
        return;
    }

    ESP_LOGI(TAG,
             "ACK received for pkt %08" PRIX32 " from %08" PRIX32 " (RSSI at dest: %d, %u hops)",
             ack.ack_packet_id, ack.src_addr, ack.rssi_at_dest, ack.hop_count);

    /* Remove from pending ACK table */
    bool found = pending_ack_remove(&s_pending_acks, ack.ack_packet_id);

    uint32_t route_hops[MSG_ROUTE_MAX_HOPS] = {0};
    uint8_t route_hop_count = 0;

    /* Relay path from ACK is dest→...→sender; normalize to sender→...→dest for UIs. */
    if (s_identity) {
        route_hops[route_hop_count++] = s_identity->address;
    }
    for (int i = ack.hop_count - 1; i >= 0 && route_hop_count < MSG_ROUTE_MAX_HOPS; i--) {
        route_hops[route_hop_count++] = ack.relay_path[i];
    }

    /* Update message store status */
    if (msg_store_update_status_with_route(ack.ack_packet_id, MSG_STATUS_DELIVERED, route_hop_count,
                                           route_hops)) {
        /* An open thread is showing this message with a pending badge; repaint it
         * so the confirmation appears without the user leaving and coming back. */
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        ui_graphics_notify(UI_EVT_MSG_STATUS);
#endif
        record_ack_delivery_event(&ack);
        /* Notify webapp with full relay path from ACK */
        char addr_buf[12];
        snprintf(addr_buf, sizeof(addr_buf), "%08" PRIX32, ack.src_addr);
        cJSON* params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "from", addr_buf);
        char pkt_buf[12];
        snprintf(pkt_buf, sizeof(pkt_buf), "%08" PRIX32, ack.ack_packet_id);
        cJSON_AddStringToObject(params, "packet_id", pkt_buf);
        cJSON_AddStringToObject(params, "status", "delivered");
        cJSON_AddNumberToObject(params, "rssi_at_dest", ack.rssi_at_dest);

        cJSON* path = cJSON_AddArrayToObject(params, "relayPath");
        char hop_buf[12];
        for (uint8_t i = 0; i < route_hop_count; i++) {
            cJSON* hop = cJSON_CreateObject();
            snprintf(hop_buf, sizeof(hop_buf), "%08" PRIX32, route_hops[i]);
            cJSON_AddStringToObject(hop, "addr", hop_buf);
            cJSON_AddNumberToObject(hop, "rssi",
                                    (i == (route_hop_count - 1)) ? ack.rssi_at_dest : 0);
            cJSON_AddItemToArray(path, hop);
        }

        rpc_notify("bramble.onAck", params);
        cJSON_Delete(params);
    }

    if (found) {
        ESP_LOGI(TAG, "Message delivered to %08" PRIX32, ack.src_addr);
    }
}

static void forward_delivery_receipt(bramble_delivery_receipt_t* receipt) {
    if (!receipt)
        return;

    if (receipt->hop_count < DELIVERY_RECEIPT_MAX_HOPS) {
        receipt->relay_path[receipt->hop_count++] = s_identity->address;
    }

    if (receipt->header.hop_limit <= 1) {
        ESP_LOGD(TAG, "Delivery receipt hop limit reached, dropping");
        return;
    }
    receipt->header.hop_limit--;

    route_entry_t* route = route_lookup(&s_routes, receipt->header.dest_addr);
    if (!route || route->state == ROUTE_BROKEN) {
        ESP_LOGW(TAG, "No route to forward delivery receipt to %08" PRIX32,
                 receipt->header.dest_addr);
        return;
    }

    /* Red-team audit: was buf[96], a hand-counted constant. Not currently
     * truncating (DELIVERY_RECEIPT_MAX_SIZE is 68 as of ws 1.3b), but
     * macro-ized for the same reason as the other TX buffers in this
     * file. */
    uint8_t buf[DELIVERY_RECEIPT_MAX_SIZE];
    esp_err_t err = bramble_delivery_receipt_serialize(receipt, buf, sizeof(buf));
    if (err != ESP_OK)
        return;

    size_t wire_len = DELIVERY_RECEIPT_MIN_SIZE + ((size_t)receipt->hop_count * 4u);
    ESP_LOGI(TAG,
             "Forwarding delivery receipt for pkt %08" PRIX32 " toward %08" PRIX32 " (%u hops)",
             receipt->orig_packet_id, receipt->header.dest_addr, receipt->hop_count);
    /* Deny behavior: a forwarded receipt is best-effort on behalf of a
     * remote sender; suppress when the RECEIPT lane is exhausted.
     *
     * TX_KIND_RECEIPT_FORWARD, not TX_KIND_RECEIPT: a relay has no retry
     * structure to defer into and cannot re-originate these bytes (the seq
     * is the originator's, so a later copy is replay-rejected downstream
     * once this hop has passed one on). It therefore keeps the blind-fire
     * behavior on LBT exhaustion, and can never see
     * TX_GATE_ERR_CHANNEL_BUSY. Same RECEIPT airtime lane either way. */
    if (mesh_tx(buf, (uint8_t)wire_len, TX_KIND_RECEIPT_FORWARD) == TX_GATE_ERR_BUDGET) {
        ESP_LOGW(TAG,
                 "Forwarded delivery receipt suppressed for pkt=%08" PRIX32
                 ": receipt airtime budget exhausted",
                 receipt->orig_packet_id);
    }
}

void handle_delivery_receipt(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    (void)snr;
    bramble_delivery_receipt_t receipt;
    if (bramble_delivery_receipt_deserialize(&receipt, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Delivery receipt deserialize failed");
        return;
    }

    /* NEW-SEC-8 (STAGED, see network_key.h): verify before acting, on
     * both branches below. */
    if (!receipt_verify(&receipt)) {
        ESP_LOGW(TAG, "Delivery receipt auth failed pkt=%08" PRIX32 " src=%08" PRIX32,
                 receipt.orig_packet_id, receipt.src_addr);
        return;
    }

    /* ws 1.3b: replay check on the authenticated signer (receipt.src_addr
     * is MAC-covered, so an attacker cannot dodge the window by mutating
     * it). Checked after receipt_verify and strictly before BOTH the
     * forward branch and the for-us effect (the broadcast delivery
     * notification), so a replayed receipt never re-notifies or forwards. */
    uint64_t receipt_seq = bramble_seq48_unpack(receipt.seq);
    if (!control_replay_ok(receipt.src_addr, receipt_seq)) {
        ESP_LOGW(TAG, "Delivery receipt replay pkt=%08" PRIX32 " src=%08" PRIX32,
                 receipt.orig_packet_id, receipt.src_addr);
        return;
    }

    if (receipt.header.dest_addr != s_identity->address) {
        forward_delivery_receipt(&receipt);
        return;
    }

    /* Write the receipt back to msg_store, exactly as handle_ack does for a
     * unicast ACK. Without this the receipt was verified, replay checked, and
     * then only emitted as a notification: the stored broadcast row kept its
     * original status forever, so a broadcast in the chat UI could never show a
     * delivery mark no matter how many nodes confirmed it. The row is keyed by
     * the packet_id we transmitted, which is what the receiver echoes back in
     * orig_packet_id.
     *
     * The receipt's relay_path runs receiver -> ... -> us; normalize it to
     * us -> ... -> receiver so the route reads the same direction as an ACK's. */
    uint32_t route_hops[MSG_ROUTE_MAX_HOPS] = {0};
    uint8_t route_hop_count = 0;
    if (s_identity) {
        route_hops[route_hop_count++] = s_identity->address;
    }
    for (int i = receipt.hop_count - 1; i >= 0 && route_hop_count < MSG_ROUTE_MAX_HOPS; i--) {
        route_hops[route_hop_count++] = receipt.relay_path[i];
    }
    if (msg_store_update_status_with_route(receipt.orig_packet_id, MSG_STATUS_DELIVERED,
                                           route_hop_count, route_hops)) {
#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        ui_graphics_notify(UI_EVT_MSG_STATUS);
#endif
    }

    /* An attested roll-call's announce is an ordinary broadcast, so the
     * receipts it draws are these same verified, replay-checked frames. When
     * orig_packet_id matches one of our announce rounds the receipt's relay
     * path lands on the responder's ledger row; otherwise this is a no-op.
     * This is the whole of the roll-call's path reporting: there is no
     * roll-call-specific telemetry frame. */
    mesh_rollcall_note_receipt(receipt.src_addr, receipt.orig_packet_id, receipt.hop_count,
                               receipt.relay_path);

    /* The originator's own record that a broadcast reached a specific node,
     * and by which route, mirroring the unicast ACK path's "Message
     * delivered to %08X" above. The console is the only place this arrival
     * is visible on a node with no RPC client attached (the emulator carries
     * no RPC transport); connected clients also get it through
     * mesh_emit_broadcast_delivery_notification below. The relay path is
     * printed in travel order, receiver first, which is the direction the
     * receipt itself carries. */
    uint8_t logged_hops = (receipt.hop_count > DELIVERY_RECEIPT_MAX_HOPS)
                              ? DELIVERY_RECEIPT_MAX_HOPS
                              : receipt.hop_count;
    /* One "XXXXXXXX" per hop plus a '>' separator between them. Written
     * nibble by nibble rather than with a running snprintf offset: an address
     * is always exactly 8 hex digits, so the bound is exact by construction
     * and needs no truncation reasoning. */
    char path_str[DELIVERY_RECEIPT_MAX_HOPS * 9 + 1];
    static const char hex_digits[] = "0123456789ABCDEF";
    size_t path_len = 0;
    for (uint8_t i = 0; i < logged_hops; i++) {
        if (i > 0)
            path_str[path_len++] = '>';
        uint32_t hop_addr = receipt.relay_path[i];
        for (int shift = 28; shift >= 0; shift -= 4)
            path_str[path_len++] = hex_digits[(hop_addr >> shift) & 0xFu];
    }
    path_str[path_len] = '\0';
    ESP_LOGI(TAG,
             "Delivery receipt from %08" PRIX32 " for broadcast %08" PRIX32
             " (%u relay hop(s)%s%s)",
             receipt.src_addr, receipt.orig_packet_id, logged_hops, logged_hops ? " via " : "",
             path_str);

    mesh_emit_broadcast_delivery_notification(receipt.src_addr, receipt.orig_packet_id, rssi,
                                              receipt.hop_count, receipt.relay_path);
}

bool mesh_supports_delivery_event_sync(void) { return true; }

uint32_t mesh_delivery_events_latest_seq(void) {
    uint32_t latest = 0u;
    if (!s_delivery_event_mutex)
        return 0u;
    xSemaphoreTake(s_delivery_event_mutex, portMAX_DELAY);
    latest = delivery_event_ring_latest_seq(s_delivery_event_ring);
    xSemaphoreGive(s_delivery_event_mutex);
    return latest;
}

size_t mesh_delivery_receipts_for_message(uint32_t message_id, uint32_t* out, size_t out_max,
                                          size_t* total_unique) {
    size_t written = 0u;
    if (total_unique)
        *total_unique = 0u;
    if (!s_delivery_event_mutex)
        return 0u;
    xSemaphoreTake(s_delivery_event_mutex, portMAX_DELAY);
    written = delivery_event_ring_receipts_for_message(s_delivery_event_ring, message_id, out,
                                                       out_max, total_unique);
    xSemaphoreGive(s_delivery_event_mutex);
    return written;
}

size_t mesh_delivery_events_list_since(uint32_t since_event_seq, delivery_event_record_t* out,
                                       size_t out_max) {
    size_t count = 0u;
    if (!s_delivery_event_mutex)
        return 0u;
    xSemaphoreTake(s_delivery_event_mutex, portMAX_DELAY);
    count = delivery_event_ring_list_since(s_delivery_event_ring, since_event_seq, out, out_max);
    xSemaphoreGive(s_delivery_event_mutex);
    return count;
}

uint32_t mesh_get_last_broadcast_id(void) { return s_last_broadcast_id; }

void mesh_set_broadcast_telemetry_mode(broadcast_telemetry_mode_t mode) {
    if (mode < BROADCAST_TELEMETRY_OFF || mode > BROADCAST_TELEMETRY_PATH_SAMPLED) {
        mode = BROADCAST_TELEMETRY_RECIPIENT_ONLY;
    }
    s_broadcast_telemetry_mode = mode;
}

broadcast_telemetry_mode_t mesh_get_broadcast_telemetry_mode(void) {
    return s_broadcast_telemetry_mode;
}

void mesh_emit_broadcast_delivery_notification(uint32_t src_addr, uint32_t broadcast_id,
                                               int8_t rssi_at_dest, uint8_t hop_count,
                                               const uint32_t* relay_path) {
    if (s_broadcast_telemetry_mode == BROADCAST_TELEMETRY_OFF) {
        return;
    }

    char src_buf[12], id_buf[12], hop_buf[12];
    cJSON* params = cJSON_CreateObject();
    snprintf(src_buf, sizeof(src_buf), "%08" PRIX32, src_addr);
    snprintf(id_buf, sizeof(id_buf), "%08" PRIX32, broadcast_id);
    cJSON_AddStringToObject(params, "recipient", src_buf);
    cJSON_AddStringToObject(params, "broadcast_id", id_buf);
    cJSON_AddStringToObject(params, "status", "delivered");
    cJSON_AddNumberToObject(params, "rssi_at_dest", rssi_at_dest);

    if (s_broadcast_telemetry_mode == BROADCAST_TELEMETRY_PATH_SAMPLED && hop_count > 0 &&
        relay_path) {
        uint8_t bounded_hops =
            (hop_count > DELIVERY_RECEIPT_MAX_HOPS) ? DELIVERY_RECEIPT_MAX_HOPS : hop_count;
        cJSON* path = cJSON_AddArrayToObject(params, "relayPath");
        for (uint8_t i = 0; i < bounded_hops; i++) {
            cJSON* hop = cJSON_CreateObject();
            snprintf(hop_buf, sizeof(hop_buf), "%08" PRIX32, relay_path[i]);
            cJSON_AddStringToObject(hop, "addr", hop_buf);
            cJSON_AddItemToArray(path, hop);
        }
    }

    record_broadcast_delivery_event(src_addr, broadcast_id, hop_count, relay_path);

    rpc_notify("bramble.onBroadcastDelivery", params);
    cJSON_Delete(params);
}
