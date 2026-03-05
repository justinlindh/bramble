#include "bramble_probe.h"
#include <string.h>

/* Simple PRNG for jitter when bramble_random is not available */
static uint32_t probe_simple_random(uint32_t min, uint32_t max) {
    static uint32_t seed = 12345;
    seed = seed * 1103515245 + 12345;
    if (max <= min)
        return min;
    return min + (seed % (max - min + 1));
}

static void rate_limit_init(probe_rate_limit_t* rl, uint8_t max_tokens, uint32_t refill_ms,
                            uint32_t now_ms) {
    rl->tokens = max_tokens;
    rl->max_tokens = max_tokens;
    rl->last_refill_ms = now_ms;
    rl->refill_interval_ms = refill_ms;
}

static void rate_limit_refill(probe_rate_limit_t* rl, uint32_t now_ms) {
    if (rl->refill_interval_ms == 0)
        return;
    uint32_t elapsed = now_ms - rl->last_refill_ms;
    uint32_t new_tokens = elapsed / rl->refill_interval_ms;
    if (new_tokens > 0) {
        rl->tokens += new_tokens;
        if (rl->tokens > rl->max_tokens)
            rl->tokens = rl->max_tokens;
        rl->last_refill_ms += new_tokens * rl->refill_interval_ms;
    }
}

static bool rate_limit_try(probe_rate_limit_t* rl, uint32_t now_ms) {
    rate_limit_refill(rl, now_ms);
    if (rl->tokens > 0) {
        rl->tokens--;
        return true;
    }
    return false;
}

static bool dedup_check(bramble_probe_state_t* state, uint32_t probe_id) {
    for (int i = 0; i < PROBE_DEDUP_SIZE; i++) {
        if (state->seen_probes[i] == probe_id)
            return true; /* already seen */
    }
    return false;
}

static void dedup_add(bramble_probe_state_t* state, uint32_t probe_id) {
    state->seen_probes[state->seen_index] = probe_id;
    state->seen_index = (state->seen_index + 1) % PROBE_DEDUP_SIZE;
}

void bramble_probe_init(bramble_probe_state_t* state, uint32_t my_addr, probe_send_fn send_fn,
                        void* send_ctx) {
    memset(state, 0, sizeof(*state));
    state->my_addr = my_addr;
    state->send_fn = send_fn;
    state->send_ctx = send_ctx;
    rate_limit_init(&state->send_limiter, PROBE_RATE_LIMIT_TOKENS, PROBE_RATE_LIMIT_REFILL_MS, 0);
}

int bramble_probe_send(bramble_probe_state_t* state, uint8_t flags, uint32_t now_ms) {
    if (!rate_limit_try(&state->send_limiter, now_ms))
        return -1;

    /* Generate probe ID from address + time */
    uint32_t probe_id = state->my_addr ^ now_ms ^ (now_ms << 16);

    bramble_probe_packet_t pkt;
    pkt.version = 1; /* BRAMBLE_VERSION */
    pkt.type = BRAMBLE_TYPE_BROADCAST_PROBE;
    pkt.flags = flags;
    pkt.hop_limit = 3;
    pkt.src_addr = state->my_addr;
    pkt.probe_id = probe_id;

    /* Start collection */
    memset(&state->result, 0, sizeof(state->result));
    state->result.probe_id = probe_id;
    state->result.start_ms = now_ms;
    state->collecting = true;

    if (state->send_fn)
        state->send_fn((const uint8_t*)&pkt, sizeof(pkt), state->send_ctx);

    return 0;
}

void bramble_probe_handle_probe(bramble_probe_state_t* state, const uint8_t* data, uint16_t len,
                                int8_t rssi, uint32_t now_ms) {
    if (len < sizeof(bramble_probe_packet_t))
        return;

    const bramble_probe_packet_t* pkt = (const bramble_probe_packet_t*)data;

    /* Don't respond to our own probes */
    if (pkt->src_addr == state->my_addr)
        return;

    /* Silent probes get no response */
    if (pkt->flags & PROBE_FLAG_SILENT)
        return;

    /* Dedup */
    if (dedup_check(state, pkt->probe_id))
        return;
    dedup_add(state, pkt->probe_id);

    /* ACK rate limiting */
    if (now_ms - state->last_ack_sent_ms < PROBE_ACK_COOLDOWN_MS)
        return;

    /* Build ACK with random jitter delay */
    bramble_probe_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.version = 1;
    ack.type = BRAMBLE_TYPE_BROADCAST_ACK;
    ack.flags = pkt->flags;
    ack.hop_count = 1;
    ack.src_addr = state->my_addr;
    ack.probe_id = pkt->probe_id;
    ack.rssi = rssi;

    uint16_t ack_len = 13; /* base size without optional rssi */
    if (pkt->flags & PROBE_FLAG_INCLUDE_RSSI)
        ack_len = 14; /* include rssi byte */

    uint32_t jitter = probe_simple_random(PROBE_ACK_JITTER_MIN_MS, PROBE_ACK_JITTER_MAX_MS);

    state->pending_ack.active = true;
    state->pending_ack.send_at_ms = now_ms + jitter;
    memcpy(&state->pending_ack.ack, &ack, sizeof(ack));
    state->pending_ack.ack_len = ack_len;
}

void bramble_probe_handle_ack(bramble_probe_state_t* state, const uint8_t* data, uint16_t len,
                              uint32_t now_ms) {
    if (len < 13)
        return;

    const bramble_probe_ack_t* ack = (const bramble_probe_ack_t*)data;

    if (!state->collecting || ack->probe_id != state->result.probe_id)
        return;

    /* Never include ourselves as a responder. */
    if (ack->src_addr == state->my_addr)
        return;

    bool has_rssi = (ack->flags & PROBE_FLAG_INCLUDE_RSSI) && len >= 14;
    uint32_t latency_ms = now_ms - state->result.start_ms;

    /* Upsert by responder address: one logical row per responder. */
    for (uint16_t i = 0; i < state->result.response_count; i++) {
        probe_response_t* existing = &state->result.responses[i];
        if (existing->responder_addr != ack->src_addr)
            continue;

        existing->hop_count = ack->hop_count;
        existing->latency_ms = latency_ms; /* latest latency */

        if (has_rssi) {
            if (!existing->has_rssi || ack->rssi > existing->rssi) {
                existing->rssi = ack->rssi; /* keep best RSSI */
            }
            existing->has_rssi = true;
        }
        return;
    }

    if (state->result.response_count >= PROBE_MAX_RESPONSES)
        return;

    probe_response_t* resp = &state->result.responses[state->result.response_count];
    resp->responder_addr = ack->src_addr;
    resp->hop_count = ack->hop_count;
    resp->latency_ms = latency_ms;
    resp->has_rssi = has_rssi;
    resp->rssi = has_rssi ? ack->rssi : 0;

    state->result.response_count++;
}

const probe_result_t* bramble_probe_get_result(const bramble_probe_state_t* state) {
    return &state->result;
}

bool bramble_probe_can_send(const bramble_probe_state_t* state, uint32_t now_ms) {
    /* Check tokens without modifying state - cast away const for refill check */
    probe_rate_limit_t tmp = state->send_limiter;
    rate_limit_refill(&tmp, now_ms);
    return tmp.tokens > 0;
}

uint32_t bramble_probe_get_rate_limit_remaining_sec(const bramble_probe_state_t* state,
                                                    uint32_t now_ms) {
    probe_rate_limit_t tmp = state->send_limiter;
    rate_limit_refill(&tmp, now_ms);
    if (tmp.tokens > 0)
        return 0;
    uint32_t elapsed = now_ms - tmp.last_refill_ms;
    if (elapsed >= tmp.refill_interval_ms)
        return 0;
    return (tmp.refill_interval_ms - elapsed + 999) / 1000;
}

void bramble_probe_tick(bramble_probe_state_t* state, uint32_t now_ms) {
    /* Send pending delayed ACK */
    if (state->pending_ack.active && now_ms >= state->pending_ack.send_at_ms) {
        if (state->send_fn)
            state->send_fn((const uint8_t*)&state->pending_ack.ack, state->pending_ack.ack_len,
                           state->send_ctx);
        state->last_ack_sent_ms = now_ms;
        state->pending_ack.active = false;
    }

    /* Check collection window expiry */
    if (state->collecting && (now_ms - state->result.start_ms) >= PROBE_COLLECTION_WINDOW_MS) {
        state->result.complete = true;
        state->collecting = false;
    }
}
