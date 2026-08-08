/**
 * mesh_rollcall.c: the attested roll-call's firmware wiring.
 *
 * The protocol itself (wire codecs, canonical signed message, stagger, round
 * schedule, rate limit, ledger) lives in components/rollcall and is shared
 * verbatim with the host suites and the simulator. This file is the glue:
 * it moves those frames through the EXISTING transports rather than adding a
 * parallel one.
 *
 *   announce   send_data_packet(0xFFFFFFFF, ..., APP_TYPE_ROLLCALL)
 *              -> a broadcast channel DATA frame: channel-key AEAD, the
 *                 network-key auth_hmac every DATA carries, the shared flood
 *                 relay (channel_flood_decide), the BROADCAST airtime lane.
 *   response   send_data_packet(initiator, ..., APP_TYPE_ROLLCALL_REPLY)
 *              -> a unicast channel DATA frame over the normal reactive
 *                 path, registered by send_data_packet in the pending-ACK
 *                 table at MSG_TIER_NORMAL like any other unicast send.
 *
 * There is no roll-call timer and no roll-call task: both the re-announce
 * rounds and the staggered responses are driven from mesh_periodic_
 * maintenance's existing 10ms tick, which is also why this module adds
 * exactly ONE static (a pointer) to the firmware's RAM. Everything else is
 * one heap block allocated the first time this node either starts a
 * roll-call or is asked to answer one, so a fleet that never uses the
 * primitive pays four bytes for it.
 */
#include "mesh_internal.h"
#include "mesh_rollcall.h"

#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"

static const char* TAG = "rollcall";

/*
 * One queued, not-yet-due response. A member answers at most one round of a
 * given roll-call (rollcall_seen_claim enforces that), so this queue only
 * ever holds concurrent roll-calls from DIFFERENT initiators. Two slots is
 * the honest bound for that: a third concurrent initiator is refused and
 * counted rather than silently displacing an answer already owed.
 */
#define ROLLCALL_PENDING_MAX 2

typedef struct {
    bool used;
    uint32_t rollcall_id;
    uint32_t initiator_addr;
    uint8_t round;
    int channel_idx;
    uint32_t due_at_ms;
} rollcall_pending_t;

typedef struct {
    /* Initiator side. */
    rollcall_ledger_t ledger;
    rollcall_rate_state_t rate;
    /* The announce packet_id of every round, so a broadcast delivery receipt
     * echoing orig_packet_id can be matched back to THIS roll-call. */
    uint32_t announce_pkt_id[ROLLCALL_MAX_ROUNDS];
    int announce_channel_idx;

    /* Member side. */
    rollcall_seen_table_t seen;
    rollcall_pending_t pending[ROLLCALL_PENDING_MAX];
    uint32_t pending_dropped; /* answers refused for a full pending queue */
} rollcall_runtime_t;

static rollcall_runtime_t* s_rc;

/* Allocate the runtime block on first use. Returns NULL when the heap cannot
 * fund it, and every caller treats NULL as "this node cannot take part in a
 * roll-call right now" rather than proceeding with partial state. */
static rollcall_runtime_t* rollcall_runtime(void) {
    if (s_rc != NULL)
        return s_rc;
    s_rc = calloc(1, sizeof(rollcall_runtime_t));
    if (s_rc == NULL) {
        ESP_LOGE(TAG, "Out of memory for roll-call state");
        return NULL;
    }
    rollcall_ledger_init(&s_rc->ledger);
    rollcall_rate_init(&s_rc->rate);
    rollcall_seen_init(&s_rc->seen);
    s_rc->announce_channel_idx = 0;
    return s_rc;
}

/* The channel a roll-call rides. The public channel (index 0) is the one
 * every member of the mesh holds by construction, which is exactly the
 * audience a roll-call is asking about; a secret channel would silently
 * shrink the question to that channel's membership without saying so. */
static const bramble_channel_t* rollcall_channel(int* idx_out) {
    if (s_num_channels <= 0)
        return NULL;
    if (idx_out != NULL)
        *idx_out = 0;
    return &s_channels[0];
}

/* ── Initiator ──────────────────────────────────────────────────────── */

/*
 * The expected set: the addresses this node holds ANCHOR-CERTIFIED pins for.
 * identity_store_handle_attestation only pins an endorsed identity once an
 * anchor is provisioned (identity_store_t.has_anchor), so on an anchored
 * node "pinned" and "admitted to this fleet" are the same set, and it is the
 * only set a "these members are missing" claim can honestly be made against.
 *
 * On an un-anchored node has_anchor is false, this returns 0, and the ledger
 * reports observed responders only. See docs/rollcall.md.
 */
static uint8_t rollcall_collect_expected(uint32_t* out, uint8_t out_cap, bool* anchored_out) {
    *anchored_out = s_identity_pins.has_anchor;
    if (!s_identity_pins.has_anchor)
        return 0;

    uint8_t n = 0;
    for (int i = 0; i < IDENTITY_STORE_CAPACITY && n < out_cap; i++) {
        const identity_pin_t* p = &s_identity_pins.entries[i];
        if (!p->used)
            continue;
        if (s_identity != NULL && p->address == s_identity->address)
            continue;
        out[n++] = p->address;
    }
    return n;
}

/* Put one announce round on the air. Returns the packet_id, or 0 when the
 * send failed (encrypt error, unprovisioned node, or an airtime denial from
 * the shared tx_gate: a roll-call yields to the budget like all other
 * traffic). */
static uint32_t rollcall_send_round(uint8_t round) {
    rollcall_runtime_t* rc = s_rc;
    if (rc == NULL || !rc->ledger.active)
        return 0;

    int ch_idx = 0;
    const bramble_channel_t* ch = rollcall_channel(&ch_idx);
    if (ch == NULL) {
        ESP_LOGE(TAG, "No channel available for the roll-call announce");
        return 0;
    }

    rollcall_announce_t ann;
    memset(&ann, 0, sizeof(ann));
    ann.rollcall_id = rc->ledger.rollcall_id;
    ann.round = round;
    ann.text_len = rc->ledger.text_len;
    memcpy(ann.text, rc->ledger.text, rc->ledger.text_len);

    uint8_t payload[ROLLCALL_ANNOUNCE_MAX_SIZE];
    size_t n = rollcall_announce_encode(&ann, payload, sizeof(payload));
    if (n == 0) {
        ESP_LOGE(TAG, "Roll-call announce encode failed (round=%u)", (unsigned)round);
        return 0;
    }

    uint32_t pkt_id = send_data_packet(0xFFFFFFFFu, payload, n, ch, APP_TYPE_ROLLCALL);
    if (pkt_id == 0) {
        ESP_LOGW(TAG, "Roll-call announce round %u not transmitted", (unsigned)round);
        return 0;
    }
    if (round >= 1 && round <= ROLLCALL_MAX_ROUNDS) {
        rc->announce_pkt_id[round - 1] = pkt_id;
    }
    rc->announce_channel_idx = ch_idx;
    ESP_LOGI(TAG, "ROLLCALL ANNOUNCE id=%08" PRIX32 " round=%u pkt=%08" PRIX32,
             rc->ledger.rollcall_id, (unsigned)round, pkt_id);
    return pkt_id;
}

int mesh_rollcall_start(const char* text, size_t text_len, uint32_t* rollcall_id_out) {
    if (s_identity == NULL)
        return MESH_ROLLCALL_ERR_INTERNAL;
    if (text_len > ROLLCALL_TEXT_MAX)
        return MESH_ROLLCALL_ERR_TEXT_TOO_LONG;

    rollcall_runtime_t* rc = rollcall_runtime();
    if (rc == NULL)
        return MESH_ROLLCALL_ERR_INTERNAL;

    uint32_t t = now_ms();
    bool ledger_open = rc->ledger.active && rc->ledger.open;
    switch (rollcall_rate_check(&rc->rate, ledger_open, t)) {
    case ROLLCALL_START_BUSY:
        return MESH_ROLLCALL_ERR_BUSY;
    case ROLLCALL_START_RATE_LIMITED:
        return MESH_ROLLCALL_ERR_RATE_LIMITED;
    case ROLLCALL_START_OK:
    default:
        break;
    }

    /* A zero id is the reserved "no roll-call" value on the wire, so redraw
     * rather than ship a frame every decoder must reject. */
    uint32_t id = next_packet_id();
    if (id == 0)
        id = 1;

    uint32_t expected[ROLLCALL_MAX_EXPECTED];
    bool anchored = false;
    uint8_t expected_count = rollcall_collect_expected(expected, ROLLCALL_MAX_EXPECTED, &anchored);

    if (!rollcall_ledger_start(&rc->ledger, id, s_identity->address, t, text, (uint8_t)text_len,
                               expected, expected_count, anchored)) {
        return MESH_ROLLCALL_ERR_INTERNAL;
    }

    if (rollcall_send_round(1) == 0) {
        /* Nothing reached the air, so nothing is owed an answer: retire the
         * ledger instead of leaving an empty one collecting for two minutes,
         * and do NOT charge the rate limiter for a roll-call that never
         * happened. */
        rollcall_ledger_init(&rc->ledger);
        return MESH_ROLLCALL_ERR_TX;
    }

    rollcall_rate_note_start(&rc->rate, t);
    rollcall_ledger_note_round(&rc->ledger, 1, t);

    if (rollcall_id_out != NULL)
        *rollcall_id_out = id;
    ESP_LOGI(TAG,
             "ROLLCALL START id=%08" PRIX32 " expected=%u anchored=%d window_ms=%" PRIu32, id,
             (unsigned)rc->ledger.expected_count, (int)anchored, rollcall_window_ms());
    return MESH_ROLLCALL_OK;
}

const rollcall_ledger_t* mesh_rollcall_ledger(void) {
    if (s_rc == NULL || !s_rc->ledger.active)
        return NULL;
    return &s_rc->ledger;
}

uint32_t mesh_rollcall_pending_dropped(void) { return (s_rc == NULL) ? 0u : s_rc->pending_dropped; }

/* Emit the completion notification. Fired exactly once, from the single
 * rollcall_ledger_maybe_close that returns true. */
static void rollcall_emit_complete(const rollcall_ledger_t* l) {
    cJSON* params = cJSON_CreateObject();
    char buf[12];
    snprintf(buf, sizeof(buf), "%08" PRIX32, l->rollcall_id);
    cJSON_AddStringToObject(params, "rollcall_id", buf);
    cJSON_AddNumberToObject(params, "responded", rollcall_ledger_responded_count(l));
    cJSON_AddNumberToObject(params, "expected", l->expected_count);
    cJSON_AddBoolToObject(params, "anchored", l->anchored);
    cJSON_AddNumberToObject(params, "rounds", l->rounds_sent);
    cJSON_AddNumberToObject(params, "unattested", l->unattested);
    rpc_notify("bramble.onRollCallComplete", params);
    cJSON_Delete(params);
}

/* ── Member ─────────────────────────────────────────────────────────── */

void mesh_rollcall_handle_announce(uint32_t src_addr, int channel_idx, const uint8_t* data,
                                   size_t data_len) {
    rollcall_announce_t ann;
    if (!rollcall_announce_decode(data, data_len, &ann)) {
        ESP_LOGW(TAG, "Malformed roll-call announce from %08" PRIX32, src_addr);
        return;
    }

    rollcall_runtime_t* rc = rollcall_runtime();
    if (rc == NULL)
        return;

    uint32_t t = now_ms();

    /* Re-announce rounds are idempotent for a member that already answered:
     * the claim is keyed on (id, initiator), NOT on the round, so rounds 2
     * and 3 cost a decode and nothing else. */
    if (!rollcall_seen_claim(&rc->seen, ann.rollcall_id, src_addr, t)) {
        ESP_LOGD(TAG, "ROLLCALL round %u already answered id=%08" PRIX32 " from %08" PRIX32,
                 (unsigned)ann.round, ann.rollcall_id, src_addr);
        return;
    }

    int slot = -1;
    for (int i = 0; i < ROLLCALL_PENDING_MAX; i++) {
        if (!rc->pending[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Counted, not silent: a node that could not answer is exactly the
         * kind of gap a roll-call exists to surface, and it must be visible
         * on the node too rather than only as a hole in someone's ledger. */
        rc->pending_dropped++;
        ESP_LOGW(TAG, "ROLLCALL pending queue full, dropping answer to %08" PRIX32, src_addr);
        return;
    }

    uint32_t delay = rollcall_response_delay_ms(s_identity->address, ann.rollcall_id,
                                                (uint8_t)neighbor_count(&s_neighbors),
                                                esp_random());
    rc->pending[slot].used = true;
    rc->pending[slot].rollcall_id = ann.rollcall_id;
    rc->pending[slot].initiator_addr = src_addr;
    rc->pending[slot].round = ann.round;
    rc->pending[slot].channel_idx = channel_idx >= 0 ? channel_idx : 0;
    rc->pending[slot].due_at_ms = t + delay;

    ESP_LOGI(TAG,
             "ROLLCALL RX id=%08" PRIX32 " round=%u from=%08" PRIX32 " answering in %" PRIu32 "ms",
             ann.rollcall_id, (unsigned)ann.round, src_addr, delay);

    /* The operator payload is surfaced to clients as its own event rather
     * than filed as a chat message: a roll-call is an operational request,
     * and putting it in a conversation thread would make an unrelated audit
     * primitive look like traffic somebody sent. */
    cJSON* params = cJSON_CreateObject();
    char buf[12];
    snprintf(buf, sizeof(buf), "%08" PRIX32, ann.rollcall_id);
    cJSON_AddStringToObject(params, "rollcall_id", buf);
    cJSON_AddStringToObject(params, "from", addr_hex(src_addr, buf, sizeof(buf)));
    cJSON_AddStringToObject(params, "text", ann.text);
    cJSON_AddNumberToObject(params, "round", ann.round);
    rpc_notify("bramble.onRollCall", params);
    cJSON_Delete(params);
}

/* Sign and transmit one queued answer. */
static void rollcall_send_pending(rollcall_pending_t* p) {
    rollcall_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.rollcall_id = p->rollcall_id;
    resp.round = p->round;
    resp.responder_addr = s_identity->address;

    uint8_t msg[ROLLCALL_MSG_SIZE];
    if (rollcall_signed_msg(p->rollcall_id, p->initiator_addr, s_identity->address, msg,
                            sizeof(msg)) == 0) {
        memset(p, 0, sizeof(*p));
        return;
    }
    if (crypto_ed25519_sign(s_identity->ed25519_private_key, msg, sizeof(msg), resp.sig) != 0) {
        ESP_LOGE(TAG, "Roll-call response signing failed");
        memset(p, 0, sizeof(*p));
        return;
    }

    uint8_t payload[ROLLCALL_RESPONSE_SIZE];
    size_t n = rollcall_response_encode(&resp, payload, sizeof(payload));
    if (n == 0) {
        memset(p, 0, sizeof(*p));
        return;
    }

    int ch_idx = p->channel_idx;
    if (ch_idx < 0 || ch_idx >= s_num_channels)
        ch_idx = 0;

    /* Unicast, so send_data_packet registers it in the pending-ACK table at
     * MSG_TIER_NORMAL: the roll-call inherits the shipped retry/backoff
     * behavior instead of growing a retry loop of its own. A zero return is
     * a real failure (budget denial included) and the answer is dropped
     * rather than retried outside that machinery. */
    uint32_t pkt_id = send_data_packet(p->initiator_addr, payload, n, &s_channels[ch_idx],
                                       APP_TYPE_ROLLCALL_REPLY);
    if (pkt_id == 0) {
        ESP_LOGW(TAG, "ROLLCALL response to %08" PRIX32 " not transmitted", p->initiator_addr);
    } else {
        ESP_LOGI(TAG, "ROLLCALL RESPONSE id=%08" PRIX32 " to=%08" PRIX32 " pkt=%08" PRIX32,
                 p->rollcall_id, p->initiator_addr, pkt_id);
    }
    memset(p, 0, sizeof(*p));
}

/* ── Initiator: response ingest ─────────────────────────────────────── */

void mesh_rollcall_handle_response(uint32_t src_addr, const uint8_t* data, size_t data_len) {
    if (s_rc == NULL || !s_rc->ledger.active)
        return;

    rollcall_response_t resp;
    if (!rollcall_response_decode(data, data_len, &resp)) {
        ESP_LOGW(TAG, "Malformed roll-call response from %08" PRIX32, src_addr);
        return;
    }
    if (resp.rollcall_id != s_rc->ledger.rollcall_id)
        return;

    /* The signature covers responder_addr, so the verifier must check the
     * field that was SIGNED, and separately confirm the envelope agrees with
     * it. A mismatch means someone relayed another node's answer under their
     * own envelope, which is not an answer from either of them. */
    if (resp.responder_addr != src_addr) {
        ESP_LOGW(TAG, "Roll-call response responder %08" PRIX32 " does not match sender %08" PRIX32,
                 resp.responder_addr, src_addr);
        rollcall_ledger_note_unattested(&s_rc->ledger);
        return;
    }

    /* Attestation needs the responder's identity key, which this node only
     * holds as a verified pin. An unpinned responder cannot be attested: its
     * answer is counted as unattested rather than recorded, because a
     * response nobody can check is not evidence. */
    const identity_pin_t* pin = identity_store_lookup(&s_identity_pins, resp.responder_addr);
    if (pin == NULL) {
        ESP_LOGW(TAG, "Roll-call response from unpinned %08" PRIX32 ", cannot attest",
                 resp.responder_addr);
        rollcall_ledger_note_unattested(&s_rc->ledger);
        return;
    }

    uint8_t msg[ROLLCALL_MSG_SIZE];
    rollcall_signed_msg(resp.rollcall_id, s_rc->ledger.initiator_addr, resp.responder_addr, msg,
                        sizeof(msg));
    if (!crypto_ed25519_verify(pin->ed25519_pub, msg, sizeof(msg), resp.sig)) {
        ESP_LOGW(TAG, "Roll-call response signature invalid from %08" PRIX32, resp.responder_addr);
        rollcall_ledger_note_unattested(&s_rc->ledger);
        return;
    }

    uint32_t t = now_ms();
    if (!rollcall_ledger_note_response(&s_rc->ledger, resp.rollcall_id, resp.responder_addr,
                                       resp.round, t)) {
        return;
    }

    ESP_LOGI(TAG, "ROLLCALL ATTESTED id=%08" PRIX32 " from=%08" PRIX32 " round=%u",
             resp.rollcall_id, resp.responder_addr, (unsigned)resp.round);

    cJSON* params = cJSON_CreateObject();
    char buf[12];
    snprintf(buf, sizeof(buf), "%08" PRIX32, resp.rollcall_id);
    cJSON_AddStringToObject(params, "rollcall_id", buf);
    cJSON_AddStringToObject(params, "address", addr_hex(resp.responder_addr, buf, sizeof(buf)));
    cJSON_AddNumberToObject(params, "round", resp.round);
    cJSON_AddNumberToObject(params, "responded", rollcall_ledger_responded_count(&s_rc->ledger));
    cJSON_AddNumberToObject(params, "expected", s_rc->ledger.expected_count);
    rpc_notify("bramble.onRollCallResponse", params);
    cJSON_Delete(params);
}

void mesh_rollcall_note_receipt(uint32_t responder_addr, uint32_t orig_packet_id, uint8_t hop_count,
                               const uint32_t* relay_path) {
    if (s_rc == NULL || !s_rc->ledger.active || !s_rc->ledger.open)
        return;

    bool matches = false;
    for (int i = 0; i < ROLLCALL_MAX_ROUNDS; i++) {
        if (s_rc->announce_pkt_id[i] != 0 && s_rc->announce_pkt_id[i] == orig_packet_id) {
            matches = true;
            break;
        }
    }
    if (!matches)
        return;

    /* The receipt's relay_path runs responder -> ... -> us; normalize it to
     * us -> ... -> responder so the ledger reads in the same direction as
     * the message store's route, which is what an operator comparing the two
     * expects. Same normalization handle_delivery_receipt already does for
     * the store. */
    uint32_t path[ROLLCALL_PATH_MAX];
    uint8_t n = 0;
    if (s_identity != NULL)
        path[n++] = s_identity->address;
    for (int i = (int)hop_count - 1; i >= 0 && n < ROLLCALL_PATH_MAX; i--) {
        path[n++] = relay_path[i];
    }

    rollcall_ledger_note_path(&s_rc->ledger, s_rc->ledger.rollcall_id, responder_addr, n, path);
}

/* ── Tick ───────────────────────────────────────────────────────────── */

void mesh_rollcall_tick(uint32_t t) {
    rollcall_runtime_t* rc = s_rc;
    if (rc == NULL)
        return;

    /* Member: due answers first, so a response is never delayed by an extra
     * tick behind the initiator's own bookkeeping. */
    for (int i = 0; i < ROLLCALL_PENDING_MAX; i++) {
        if (rc->pending[i].used && (int32_t)(t - rc->pending[i].due_at_ms) >= 0) {
            rollcall_send_pending(&rc->pending[i]);
        }
    }

    if (!rc->ledger.active)
        return;

    if (rollcall_ledger_round_due(&rc->ledger, t)) {
        uint8_t round = (uint8_t)(rc->ledger.rounds_sent + 1);
        /* A denied or failed round still advances the schedule: the round
         * count is a hard airtime bound, and retrying a round the budget
         * refused would spend exactly the airtime the refusal protected. */
        rollcall_send_round(round);
        rollcall_ledger_note_round(&rc->ledger, round, t);
    }

    if (rollcall_ledger_maybe_close(&rc->ledger, t)) {
        ESP_LOGI(TAG,
                 "ROLLCALL COMPLETE id=%08" PRIX32 " responded=%u expected=%u unattested=%" PRIu32,
                 rc->ledger.rollcall_id, (unsigned)rollcall_ledger_responded_count(&rc->ledger),
                 (unsigned)rc->ledger.expected_count, rc->ledger.unattested);
        rollcall_emit_complete(&rc->ledger);
    }
}
