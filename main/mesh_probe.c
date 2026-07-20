/**
 * mesh_probe.c: Neighbor probe sweep and the jittered probe-reply queue.
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

static const char* TAG = "mesh";

/* Forward declarations for intra-module static helpers. */
static void mesh_schedule_next_probe_reply_timer(void);
static void queue_probe_reply(const uint8_t* buf, uint8_t wire_len, uint32_t address);

static void mesh_schedule_next_probe_reply_timer(void) {
    if (!s_probe_reply_timer)
        return;

    uint32_t earliest_due = 0;
    if (!probe_reply_queue_earliest_due(s_probe_reply_queue, PROBE_REPLY_QUEUE_CAPACITY,
                                        &earliest_due)) {
        esp_timer_stop(s_probe_reply_timer);
        return;
    }

    uint32_t t = now_ms();
    uint32_t delay_ms = (earliest_due <= t) ? 1u : (earliest_due - t);
    esp_timer_stop(s_probe_reply_timer);
    esp_err_t err = esp_timer_start_once(s_probe_reply_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to arm probe reply timer: %d", (int)err);
    }
}

static void queue_probe_reply(const uint8_t* buf, uint8_t wire_len, uint32_t address) {
    uint32_t jitter_ms = esp_random() % 120u; /* +0..119, as before */
    uint32_t initial_delay_ms = probe_reply_initial_delay_ms(address, jitter_ms);
    uint32_t first_due_ms = probe_reply_attempt_due_ms(now_ms(), initial_delay_ms, 0);

    int slot = probe_reply_queue_insert(s_probe_reply_queue, PROBE_REPLY_QUEUE_CAPACITY, buf,
                                        wire_len, PROBE_REPLY_ATTEMPTS, first_due_ms);
    if (slot < 0) {
        ESP_LOGW(TAG, "Probe reply queue full; dropping reply");
        return;
    }
    mesh_schedule_next_probe_reply_timer();
}

void mesh_process_probe_reply_tx_event(void) {
    uint32_t t_now = now_ms();
    int due_idx =
        probe_reply_queue_find_due(s_probe_reply_queue, PROBE_REPLY_QUEUE_CAPACITY, t_now);
    if (due_idx < 0) {
        mesh_schedule_next_probe_reply_timer();
        return;
    }

    pending_probe_reply_t* item = &s_probe_reply_queue[due_idx];

    /* TX can block for CAD/LBT; feed the task WDT just before entering it. */
    esp_task_wdt_reset();

    int rc = mesh_tx(item->buf, item->wire_len, TX_KIND_PROBE_REPLY);
    uint32_t t_after = now_ms(); /* measure the 140ms retry gap from TX return, matching the
                                    original vTaskDelay(140)-after-tx */

    /* Deny-stop vs. sent-and-retry decision lives in the pure state machine.
     * TX_GATE_ERR_BUDGET abandons the whole reply (first thing to shed);
     * otherwise the send is counted and the next attempt is scheduled
     * t_after + 140ms until attempts_total is reached. */
    probe_reply_tx_result_t result =
        (rc == TX_GATE_ERR_BUDGET) ? PROBE_REPLY_TX_DENIED : PROBE_REPLY_TX_SENT;
    probe_reply_queue_apply_result(item, result, t_after);

    mesh_schedule_next_probe_reply_timer();
}

void mesh_probe_reply_timer_cb(void* arg) {
    (void)arg;
    if (!s_mesh_event_queue)
        return;

    mesh_event_type_t evt = MESH_EVT_PROBE_REPLY_TX;
    if (xQueueSend(s_mesh_event_queue, &evt, 0) != pdTRUE) {
        ESP_EARLY_LOGW(TAG, "mesh event queue full; dropped probe reply timer event");
    }
}

void mesh_get_probe_ingress_stats(uint32_t* accepted, uint32_t* dropped_reply,
                                  uint32_t* dropped_forward) {
    if (accepted)
        *accepted = s_probe_ingress.accepted;
    if (dropped_reply)
        *dropped_reply = s_probe_ingress.dropped_reply;
    if (dropped_forward)
        *dropped_forward = s_probe_ingress.dropped_forward;
}

/* ── Probe tracking ──────────────────────────────────────────────────── */

int mesh_send_probe_round(uint32_t pid, uint8_t round) {
    /* Probe packet: header(12) + source_addr(4) + round(1) */
    uint8_t buf[20];
    bramble_header_t header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_PROBE,
        .flags = 0,
        .hop_limit = 8,
        .dest_addr = 0xFFFFFFFF,
        .packet_id = pid,
    };
    bramble_header_serialize(&header, buf, HEADER_SIZE);
    memcpy(buf + HEADER_SIZE, &s_identity->address, 4);
    buf[HEADER_SIZE + 4] = round;

    /* Deny behavior: a probe sweep is on-demand diagnostics; a denied
     * round is simply skipped and reported via the per-round rc log. */
    int rc = mesh_tx(buf, HEADER_SIZE + 5, TX_KIND_PROBE);
    ESP_LOGI(TAG, "PROBE SWEEP TX pid=%08" PRIX32 " round=%u rc=%d", pid, (unsigned)round, rc);
    return rc;
}

void mesh_start_probe_sweep(uint32_t pid) {
    s_probe_id = pid;
    s_probe_sent_ms = now_ms();
    s_probe_result_count = 0;
    s_probe_collecting = true;
    s_probe_complete_emitted = false;
    s_probe_rounds_sent = 0;
    s_probe_next_round_ms = s_probe_sent_ms;

    /* Round 1 immediate; rounds 2..N sent by periodic maintenance. */
    mesh_send_probe_round(pid, 1);
    s_probe_rounds_sent = 1;
    s_probe_next_round_ms = s_probe_sent_ms + PROBE_SWEEP_INTERVAL_MS;

    ESP_LOGI(TAG, "PROBE SWEEP START pid=%08" PRIX32 " rounds=%u interval_ms=%u", pid,
             (unsigned)PROBE_SWEEP_ROUNDS, (unsigned)PROBE_SWEEP_INTERVAL_MS);
}

uint32_t mesh_send_probe(void) {
    uint32_t pid = next_packet_id();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_probe_request_pending || s_probe_collecting) {
        xSemaphoreGive(s_state_mutex);
        ESP_LOGW(TAG, "PROBE request ignored: busy (pending=%d collecting=%d)",
                 s_probe_request_pending, s_probe_collecting);
        return 0;
    }
    s_probe_request_pending = true;
    s_probe_request_id = pid;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "PROBE SWEEP QUEUED pid=%08" PRIX32, pid);
    return pid;
}

void handle_probe(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    if (len < HEADER_SIZE + 4)
        return;

    bramble_header_t header;
    bramble_header_deserialize(&header, data, len);

    uint32_t src_addr;
    memcpy(&src_addr, data + HEADER_SIZE, 4);
    uint8_t probe_round = (len >= HEADER_SIZE + 5) ? data[HEADER_SIZE + 4] : 1;

    char src_buf[12], me_buf[12];
    /* Debug, not info: this fires once per received PROBE, before the ingress
     * rate limit below has any say. PROBE is unauthenticated and remotely
     * inducible, so an attacker in radio range could otherwise buy one UART
     * line per injected frame and starve the serial RPC channel a maintainer
     * would reach for while diagnosing the flood. Same reasoning as commit
     * 843db077, which demoted the raw NMEA log for exactly this failure mode
     * (issue #174). */
    ESP_LOGD(TAG, "PROBE RX pid=%08" PRIX32 " round=%u src=%s me=%s hop=%u rssi=%d snr=%d",
             header.packet_id, (unsigned)probe_round, addr_hex(src_addr, src_buf, sizeof(src_buf)),
             addr_hex(s_identity->address, me_buf, sizeof(me_buf)), (unsigned)header.hop_limit,
             (int)rssi, (int)snr);

    /* Ignore our own probe if it loops back through relays. */
    if (src_addr == s_identity->address) {
        /* Also pre-rate-limit and forgeable (src_addr is unauthenticated), so
         * keep it at debug for the same reason as the line above. */
        ESP_LOGD(TAG, "PROBE RX ignored self-originated pid=%08" PRIX32, header.packet_id);
        return;
    }

    /* Ingress backpressure (issue #75). PROBE is unauthenticated on purpose:
     * an unprovisioned node asking "who can hear me" is the feature, so
     * there is deliberately no MAC check and no provisioning gate here. What
     * is bounded is the AMPLIFICATION. Accepting a probe costs this node a
     * three-send reply burst plus a rebroadcast, and dedup keys on a
     * packet_id an attacker varies freely, so before this the cost ratio of
     * one injected 16-byte frame to four transmissions per node in earshot
     * was unbounded. The cap is node-global, NOT keyed on the src_addr above:
     * that field is unauthenticated, so per-sender keying would be evadable
     * by rotating it and would hand an attacker a targeted DoS against any
     * victim whose address it forged. Same call, same reasons, as SEC-M4's
     * forwarded-RREQ cap; security.h has the full argument.
     *
     * Forward eligibility is passed IN rather than inferred: a probe that
     * arrived hop-exhausted was never going to propagate, so it must not
     * debit the tighter forward bucket. Probes originate at hop_limit 8, so
     * every legitimate sweep ends with hop_limit 1 arrivals at the edge of
     * range; charging those would let ordinary traffic suppress forwarding
     * for genuinely eligible multi-hop probes. */
    bool forward_eligible = header.hop_limit > 1;
    probe_ingress_decision_t ingress =
        probe_ingress_allow(&s_probe_ingress, forward_eligible, now_ms());
    if (!ingress.reply) {
        ESP_LOGW(TAG, "PROBE RX rate limited pid=%08" PRIX32 " src=%s (drops=%" PRIu32 ")",
                 header.packet_id, addr_hex(src_addr, src_buf, sizeof(src_buf)),
                 s_probe_ingress.dropped_reply);
        return;
    }

    /* Send probe ACK back */
    uint8_t buf[20];
    bramble_header_t ack_header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_PROBE_ACK,
        .flags = 0,
        .hop_limit = 8,
        .dest_addr = src_addr,
        .packet_id = header.packet_id,
    };
    bramble_header_serialize(&ack_header, buf, HEADER_SIZE);
    memcpy(buf + HEADER_SIZE, &s_identity->address, 4);
    buf[HEADER_SIZE + 4] = 1; /* hops = 1 for direct */
    buf[HEADER_SIZE + 5] = probe_round;

    /* Defer the reply burst (slot delay + jitter + 3 sends 140ms apart) onto
     * the probe-reply timer/queue so the mesh task is never blocked (DES-15).
     * Same slotting/jitter/spacing as before; only the blocking is removed. */
    queue_probe_reply(buf, HEADER_SIZE + 6, s_identity->address);

    ESP_LOGI(TAG, "PROBE ACK QUEUED pid=%08" PRIX32 " round=%u to=%s from=%s hops=1",
             header.packet_id, (unsigned)probe_round, addr_hex(src_addr, src_buf, sizeof(src_buf)),
             addr_hex(s_identity->address, me_buf, sizeof(me_buf)));

    /* Forward probe if the hop limit allows AND the forward bucket agreed.
     * The reply above is a bounded local cost; the rebroadcast is what turns
     * one injected frame into mesh-wide traffic, so it runs out of budget
     * first and stops propagation while this node keeps answering its
     * neighbors. A hop-exhausted probe falls out here having spent nothing
     * from the forward bucket. */
    if (forward_eligible) {
        if (!ingress.forward) {
            ESP_LOGW(TAG,
                     "PROBE FWD suppressed by ingress budget pid=%08" PRIX32 " (drops=%" PRIu32 ")",
                     header.packet_id, s_probe_ingress.dropped_forward);
        } else {
            bramble_header_t fwd = header;
            fwd.hop_limit--;
            uint8_t fwd_buf[20];
            bramble_header_serialize(&fwd, fwd_buf, HEADER_SIZE);
            memcpy(fwd_buf + HEADER_SIZE, data + HEADER_SIZE, 4);
            mesh_tx(fwd_buf, HEADER_SIZE + 4, TX_KIND_PROBE);
            ESP_LOGI(TAG, "PROBE FWD pid=%08" PRIX32 " new_hop=%u", header.packet_id,
                     (unsigned)fwd.hop_limit);
        }
    }
}

void handle_probe_ack(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    if (len < HEADER_SIZE + 5)
        return;

    bramble_header_t header;
    bramble_header_deserialize(&header, data, len);

    char dst_buf[12];

    /* If ACK is not for us, forward it (multi-hop probe result relay). */
    if (header.dest_addr != s_identity->address) {
        if (header.hop_limit > 1) {
            bramble_header_t fwd = header;
            fwd.hop_limit--;
            uint8_t fwd_buf[BRAMBLE_MAX_PACKET_SIZE];
            memcpy(fwd_buf, data, len);
            bramble_header_serialize(&fwd, fwd_buf, HEADER_SIZE);
            mesh_tx(fwd_buf, len, TX_KIND_PROBE_REPLY);
            ESP_LOGI(TAG, "PROBE ACK FWD pid=%08" PRIX32 " dest=%s hop=%u", header.packet_id,
                     addr_hex(header.dest_addr, dst_buf, sizeof(dst_buf)), (unsigned)fwd.hop_limit);
        } else {
            ESP_LOGI(TAG, "PROBE ACK drop hop-limit pid=%08" PRIX32, header.packet_id);
        }
        return;
    }

    /* Only process if this ACK is for our active probe */
    if (!s_probe_collecting || header.packet_id != s_probe_id) {
        return;
    }

    uint32_t resp_addr;
    memcpy(&resp_addr, data + HEADER_SIZE, 4);
    uint8_t hops = data[HEADER_SIZE + 4];
    uint8_t probe_round = (len >= HEADER_SIZE + 6) ? data[HEADER_SIZE + 5] : 1;
    if (probe_round < 1 || probe_round > PROBE_SWEEP_ROUNDS)
        probe_round = 1;

    /* Never include self in probe responders. */
    if (resp_addr == s_identity->address) {
        ESP_LOGI(TAG, "PROBE ACK RX ignored self responder pid=%08" PRIX32, header.packet_id);
        return;
    }

    uint32_t latency = now_ms() - s_probe_sent_ms;

    /* Upsert by responder addr: one logical row per responder. */
    probe_results_upsert(s_probe_results, &s_probe_result_count, MAX_PROBE_RESULTS, resp_addr, hops,
                         rssi, snr, latency, probe_round);

    char buf[12];
    ESP_LOGI(TAG,
             "PROBE ACK RX from=%s round=%u hops=%u rssi=%d snr=%d latency=%" PRIu32
             "ms pid=%08" PRIX32,
             addr_hex(resp_addr, buf, sizeof(buf)), (unsigned)probe_round, (unsigned)hops,
             (int)rssi, (int)snr, now_ms() - s_probe_sent_ms, header.packet_id);

    /* Emit notification */
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "address", addr_hex(resp_addr, buf, sizeof(buf)));
    cJSON_AddNumberToObject(params, "hops", hops);
    cJSON_AddNumberToObject(params, "rssi", rssi);
    cJSON_AddNumberToObject(params, "snr", snr);
    cJSON_AddNumberToObject(params, "latency_ms", latency);
    cJSON_AddNumberToObject(params, "probe_round", probe_round);
    char pid_buf[12];
    snprintf(pid_buf, sizeof(pid_buf), "%08" PRIX32, s_probe_id);
    cJSON_AddStringToObject(params, "probe_id", pid_buf);
    rpc_notify("bramble.onProbeResult", params);
    cJSON_Delete(params);
}
