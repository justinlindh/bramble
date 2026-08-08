/**
 * rollcall.c: the attested roll-call's pure protocol core.
 *
 * See rollcall.h for the primitive's contract. Nothing here reads a clock,
 * touches a radio, allocates, or picks a crypto backend: every input arrives
 * as a parameter so the firmware, the host suites and the Go simulator drive
 * identical logic.
 */
#include "rollcall.h"

#include <string.h>

/* ── Big-endian helpers ─────────────────────────────────────────────────
 * Local rather than shared with packet.c's put_be32: this component
 * deliberately has no dependency on the packet framing (it encodes an inner
 * PAYLOAD, not a frame), and duplicating four shifts is cheaper than a
 * header dependency that would drag the whole wire layout in. */

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ── Wire ───────────────────────────────────────────────────────────── */

size_t rollcall_announce_encode(const rollcall_announce_t* in, uint8_t* out, size_t out_cap) {
    if (in == NULL || out == NULL)
        return 0;
    if (in->rollcall_id == 0)
        return 0;
    if (in->round < 1 || in->round > ROLLCALL_MAX_ROUNDS)
        return 0;
    if (in->text_len > ROLLCALL_TEXT_MAX)
        return 0;

    size_t need = (size_t)ROLLCALL_ANNOUNCE_HEADER_SIZE + in->text_len;
    if (out_cap < need)
        return 0;

    put_be32(out, in->rollcall_id);
    out[4] = in->round;
    out[5] = in->text_len;
    if (in->text_len > 0)
        memcpy(out + ROLLCALL_ANNOUNCE_HEADER_SIZE, in->text, in->text_len);
    return need;
}

bool rollcall_announce_decode(const uint8_t* buf, size_t len, rollcall_announce_t* out) {
    if (buf == NULL || out == NULL)
        return false;
    if (len < ROLLCALL_ANNOUNCE_HEADER_SIZE)
        return false;

    uint32_t id = get_be32(buf);
    uint8_t round = buf[4];
    uint8_t text_len = buf[5];

    if (id == 0)
        return false;
    if (round < 1 || round > ROLLCALL_MAX_ROUNDS)
        return false;
    if (text_len > ROLLCALL_TEXT_MAX)
        return false;
    /* Exact-length check, not >=: a frame carrying more bytes than its own
     * text_len declares is malformed, and accepting it would let a sender
     * smuggle unauthenticated trailing bytes past every downstream length
     * assumption. Same rule the attestation deserializer applies. */
    if (len != (size_t)ROLLCALL_ANNOUNCE_HEADER_SIZE + text_len)
        return false;

    memset(out, 0, sizeof(*out));
    out->rollcall_id = id;
    out->round = round;
    out->text_len = text_len;
    if (text_len > 0)
        memcpy(out->text, buf + ROLLCALL_ANNOUNCE_HEADER_SIZE, text_len);
    out->text[text_len] = '\0';
    return true;
}

size_t rollcall_response_encode(const rollcall_response_t* in, uint8_t* out, size_t out_cap) {
    if (in == NULL || out == NULL)
        return 0;
    if (in->rollcall_id == 0)
        return 0;
    if (in->round < 1 || in->round > ROLLCALL_MAX_ROUNDS)
        return 0;
    if (out_cap < ROLLCALL_RESPONSE_SIZE)
        return 0;

    put_be32(out, in->rollcall_id);
    out[4] = in->round;
    put_be32(out + 5, in->responder_addr);
    memcpy(out + 9, in->sig, ROLLCALL_SIG_SIZE);
    return ROLLCALL_RESPONSE_SIZE;
}

bool rollcall_response_decode(const uint8_t* buf, size_t len, rollcall_response_t* out) {
    if (buf == NULL || out == NULL)
        return false;
    if (len != ROLLCALL_RESPONSE_SIZE)
        return false;

    uint32_t id = get_be32(buf);
    uint8_t round = buf[4];
    if (id == 0)
        return false;
    if (round < 1 || round > ROLLCALL_MAX_ROUNDS)
        return false;

    memset(out, 0, sizeof(*out));
    out->rollcall_id = id;
    out->round = round;
    out->responder_addr = get_be32(buf + 5);
    memcpy(out->sig, buf + 9, ROLLCALL_SIG_SIZE);
    return true;
}

size_t rollcall_signed_msg(uint32_t rollcall_id, uint32_t initiator_addr, uint32_t responder_addr,
                           uint8_t* out, size_t out_cap) {
    if (out == NULL || out_cap < ROLLCALL_MSG_SIZE)
        return 0;
    memcpy(out, ROLLCALL_MSG_CONTEXT, ROLLCALL_MSG_CONTEXT_LEN);
    put_be32(out + ROLLCALL_MSG_CONTEXT_LEN, rollcall_id);
    put_be32(out + ROLLCALL_MSG_CONTEXT_LEN + 4, initiator_addr);
    put_be32(out + ROLLCALL_MSG_CONTEXT_LEN + 8, responder_addr);
    return ROLLCALL_MSG_SIZE;
}

/* ── Response stagger ───────────────────────────────────────────────── */

uint32_t rollcall_response_slot(uint32_t responder_addr, uint32_t rollcall_id, uint8_t peer_count) {
    uint32_t buckets = (uint32_t)peer_count * 2u;
    if (buckets < ROLLCALL_SLOT_BUCKETS_MIN)
        buckets = ROLLCALL_SLOT_BUCKETS_MIN;
    if (buckets > ROLLCALL_SLOT_BUCKETS_MAX)
        buckets = ROLLCALL_SLOT_BUCKETS_MAX;
    return (responder_addr ^ rollcall_id) % buckets;
}

uint32_t rollcall_response_delay_ms(uint32_t responder_addr, uint32_t rollcall_id,
                                    uint8_t peer_count, uint32_t random_value) {
    uint32_t slot = rollcall_response_slot(responder_addr, rollcall_id, peer_count);
    uint32_t jitter = random_value % ROLLCALL_RESPONSE_JITTER_MS;
    return ROLLCALL_RESPONSE_BASE_MS + slot * ROLLCALL_RESPONSE_SLOT_MS + jitter;
}

/* ── Round schedule ─────────────────────────────────────────────────── */

uint32_t rollcall_round_delay_ms(uint8_t rounds_sent) {
    if (rounds_sent < 1 || rounds_sent >= ROLLCALL_MAX_ROUNDS)
        return 0;
    /* 30s after round 1, 60s after round 2. The shift is bounded by the
     * ROLLCALL_MAX_ROUNDS guard above, so it can never run off the type. */
    return ROLLCALL_ROUND_BASE_MS << (rounds_sent - 1);
}

uint32_t rollcall_window_ms(void) {
    uint32_t total = 0;
    for (uint8_t r = 1; r < ROLLCALL_MAX_ROUNDS; r++) {
        total += rollcall_round_delay_ms(r);
    }
    return total + ROLLCALL_COLLECT_TAIL_MS;
}

/* ── Initiation rate limit ──────────────────────────────────────────── */

void rollcall_rate_init(rollcall_rate_state_t* rl) {
    if (rl == NULL)
        return;
    memset(rl, 0, sizeof(*rl));
}

rollcall_start_check_t rollcall_rate_check(const rollcall_rate_state_t* rl, bool ledger_open,
                                           uint32_t now_ms) {
    if (rl == NULL)
        return ROLLCALL_START_RATE_LIMITED;
    if (ledger_open)
        return ROLLCALL_START_BUSY;
    if (!rl->ever_started)
        return ROLLCALL_START_OK;
    /* Unsigned difference: wrap-safe across the uint32 millisecond rollover,
     * the same comparison every other deadline in the tree uses. */
    if ((now_ms - rl->last_start_ms) < ROLLCALL_MIN_INTERVAL_MS)
        return ROLLCALL_START_RATE_LIMITED;
    return ROLLCALL_START_OK;
}

void rollcall_rate_note_start(rollcall_rate_state_t* rl, uint32_t now_ms) {
    if (rl == NULL)
        return;
    rl->ever_started = true;
    rl->last_start_ms = now_ms;
}

/* ── Member side ────────────────────────────────────────────────────── */

void rollcall_seen_init(rollcall_seen_table_t* t) {
    if (t == NULL)
        return;
    memset(t, 0, sizeof(*t));
}

bool rollcall_seen_contains(const rollcall_seen_table_t* t, uint32_t rollcall_id,
                            uint32_t initiator_addr) {
    if (t == NULL)
        return false;
    for (int i = 0; i < ROLLCALL_SEEN_MAX; i++) {
        if (t->entries[i].used && t->entries[i].rollcall_id == rollcall_id &&
            t->entries[i].initiator_addr == initiator_addr) {
            return true;
        }
    }
    return false;
}

bool rollcall_seen_claim(rollcall_seen_table_t* t, uint32_t rollcall_id, uint32_t initiator_addr,
                         uint32_t now_ms) {
    if (t == NULL || rollcall_id == 0)
        return false;
    if (rollcall_seen_contains(t, rollcall_id, initiator_addr))
        return false;

    int slot = -1;
    for (int i = 0; i < ROLLCALL_SEEN_MAX; i++) {
        if (!t->entries[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Recycle the oldest by seen_ms. The subtraction (rather than a
         * direct <) keeps the choice correct across the millisecond
         * rollover: it compares AGE relative to now, not absolute stamps. */
        uint32_t oldest_age = 0;
        slot = 0;
        for (int i = 0; i < ROLLCALL_SEEN_MAX; i++) {
            uint32_t age = now_ms - t->entries[i].seen_ms;
            if (age >= oldest_age) {
                oldest_age = age;
                slot = i;
            }
        }
    }

    t->entries[slot].used = true;
    t->entries[slot].rollcall_id = rollcall_id;
    t->entries[slot].initiator_addr = initiator_addr;
    t->entries[slot].seen_ms = now_ms;
    return true;
}

void rollcall_seen_release(rollcall_seen_table_t* t, uint32_t rollcall_id,
                           uint32_t initiator_addr) {
    if (t == NULL)
        return;
    for (int i = 0; i < ROLLCALL_SEEN_MAX; i++) {
        if (t->entries[i].used && t->entries[i].rollcall_id == rollcall_id &&
            t->entries[i].initiator_addr == initiator_addr) {
            memset(&t->entries[i], 0, sizeof(t->entries[i]));
            return;
        }
    }
}

/* ── Member side: the answer budget ─────────────────────────────────── */

void rollcall_answer_budget_init(rollcall_answer_budget_t* b) {
    if (b == NULL)
        return;
    memset(b, 0, sizeof(*b));
}

uint8_t rollcall_answer_budget_used(const rollcall_answer_budget_t* b, uint32_t now_ms) {
    if (b == NULL)
        return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < ROLLCALL_ANSWER_MAX_PER_HOUR; i++) {
        /* Unsigned difference: wrap-safe across the millisecond rollover. */
        if (b->used[i] && (now_ms - b->at_ms[i]) < ROLLCALL_ANSWER_WINDOW_MS)
            n++;
    }
    return n;
}

bool rollcall_answer_budget_allow(const rollcall_answer_budget_t* b, uint32_t now_ms) {
    if (b == NULL)
        return false;
    return rollcall_answer_budget_used(b, now_ms) < ROLLCALL_ANSWER_MAX_PER_HOUR;
}

void rollcall_answer_budget_note(rollcall_answer_budget_t* b, uint32_t now_ms) {
    if (b == NULL)
        return;
    /* Prefer a free or already-expired slot; the array is exactly the cap, so
     * one exists whenever allow() said yes. The oldest-by-age fallback keeps
     * the function total for a caller that skipped the check rather than
     * leaving the newest answer unrecorded. */
    uint8_t oldest = 0;
    uint32_t oldest_age = 0;
    for (uint8_t i = 0; i < ROLLCALL_ANSWER_MAX_PER_HOUR; i++) {
        uint32_t age = now_ms - b->at_ms[i];
        if (!b->used[i] || age >= ROLLCALL_ANSWER_WINDOW_MS) {
            b->used[i] = true;
            b->at_ms[i] = now_ms;
            return;
        }
        if (age >= oldest_age) {
            oldest_age = age;
            oldest = i;
        }
    }
    b->used[oldest] = true;
    b->at_ms[oldest] = now_ms;
}

/* ── Initiator side: the ledger ─────────────────────────────────────── */

void rollcall_ledger_init(rollcall_ledger_t* l) {
    if (l == NULL)
        return;
    memset(l, 0, sizeof(*l));
}

bool rollcall_ledger_start(rollcall_ledger_t* l, uint32_t rollcall_id, uint32_t initiator_addr,
                           uint32_t now_ms, const char* text, uint8_t text_len,
                           const uint32_t* expected, uint8_t expected_count, bool anchored) {
    if (l == NULL || rollcall_id == 0)
        return false;
    if (text_len > ROLLCALL_TEXT_MAX)
        return false;
    if (text_len > 0 && text == NULL)
        return false;

    memset(l, 0, sizeof(*l));
    l->active = true;
    l->open = true;
    l->rollcall_id = rollcall_id;
    l->initiator_addr = initiator_addr;
    l->started_ms = now_ms;
    l->next_round_ms = now_ms; /* round 1 is due immediately */
    l->text_len = text_len;
    if (text_len > 0)
        memcpy(l->text, text, text_len);
    l->text[text_len] = '\0';

    /* An expected set is only meaningful when it is anchor-certified: on an
     * un-anchored mesh the pins are trust-on-first-use and free to mint, so
     * calling any address "missing" would be a claim the mesh cannot back.
     * The set is dropped rather than stored in that case, so no later code
     * path can accidentally report against it. */
    if (anchored && expected != NULL && expected_count > 0) {
        uint8_t n = expected_count;
        if (n > ROLLCALL_MAX_EXPECTED)
            n = ROLLCALL_MAX_EXPECTED;
        for (uint8_t i = 0; i < n; i++) {
            if (expected[i] == 0 || expected[i] == initiator_addr)
                continue; /* self and the null address are never expected */
            l->expected[l->expected_count++] = expected[i];
        }
        l->anchored = true;
    }
    return true;
}

uint32_t rollcall_ledger_note_round(rollcall_ledger_t* l, uint8_t round, uint32_t now_ms) {
    if (l == NULL || !l->active)
        return 0;
    l->rounds_sent = round;
    uint32_t delay = rollcall_round_delay_ms(round);
    l->next_round_ms = (delay == 0) ? 0 : now_ms + delay;
    return l->next_round_ms;
}

bool rollcall_ledger_round_due(const rollcall_ledger_t* l, uint32_t now_ms) {
    if (l == NULL || !l->active || !l->open)
        return false;
    if (l->rounds_sent >= ROLLCALL_MAX_ROUNDS)
        return false;
    if (l->rounds_sent > 0 && l->next_round_ms == 0)
        return false;
    return (int32_t)(now_ms - l->next_round_ms) >= 0;
}

uint32_t rollcall_ledger_close_at(const rollcall_ledger_t* l) {
    if (l == NULL || !l->active)
        return 0;
    return l->started_ms + rollcall_window_ms();
}

bool rollcall_ledger_maybe_close(rollcall_ledger_t* l, uint32_t now_ms) {
    if (l == NULL || !l->active || !l->open)
        return false;
    if ((int32_t)(now_ms - rollcall_ledger_close_at(l)) < 0)
        return false;
    l->open = false;
    l->closed_ms = now_ms;
    return true;
}

static rollcall_entry_t* entry_find(rollcall_ledger_t* l, uint32_t addr) {
    for (uint8_t i = 0; i < l->entry_count; i++) {
        if (l->entries[i].used && l->entries[i].addr == addr)
            return &l->entries[i];
    }
    return NULL;
}

/*
 * Find an existing row, or allocate one. Returns NULL when the table is full
 * and nothing may be taken for this caller; the caller decides which counter
 * that is (overflow for a response, a plain refusal for a path).
 *
 * for_response is the priority switch. An attested answer is the ledger's
 * output and a relay path is decoration, so when the table is full an answer
 * reclaims a path-only row rather than being turned away. Without that rule
 * the row an answer needs can already be occupied by a receipt that arrived
 * first (receipts are due at 300ms plus a slot, answers at 400ms plus up to
 * 19.5s of stagger), and a member that demonstrably answered would be
 * reported missing.
 */
static rollcall_entry_t* entry_upsert(rollcall_ledger_t* l, uint32_t addr, bool for_response) {
    rollcall_entry_t* e = entry_find(l, addr);
    if (e != NULL)
        return e;
    if (l->entry_count < ROLLCALL_MAX_RESPONDERS) {
        e = &l->entries[l->entry_count++];
        memset(e, 0, sizeof(*e));
        e->used = true;
        e->addr = addr;
        return e;
    }
    if (!for_response)
        return NULL;
    for (uint8_t i = 0; i < l->entry_count; i++) {
        if (l->entries[i].used && !l->entries[i].responded) {
            e = &l->entries[i];
            memset(e, 0, sizeof(*e));
            e->used = true;
            e->addr = addr;
            return e;
        }
    }
    return NULL;
}

bool rollcall_ledger_note_response(rollcall_ledger_t* l, uint32_t rollcall_id,
                                   uint32_t responder_addr, uint8_t round, uint32_t now_ms) {
    if (l == NULL || !l->active)
        return false;
    if (rollcall_id != l->rollcall_id)
        return false;
    if (responder_addr == 0 || responder_addr == l->initiator_addr)
        return false;
    if (!l->open) {
        l->late++;
        return false;
    }

    rollcall_entry_t* e = entry_upsert(l, responder_addr, true);
    if (e == NULL) {
        l->overflow++;
        return false;
    }
    if (e->responded) {
        /* Idempotent repeat: a retransmitted response must not move the
         * recorded time, or a member that answered promptly would read as
         * having answered whenever its last retry happened to land. */
        return true;
    }
    e->responded = true;
    e->responded_at_ms = now_ms;
    e->round = round;
    return true;
}

void rollcall_ledger_note_unattested(rollcall_ledger_t* l) {
    if (l == NULL || !l->active)
        return;
    l->unattested++;
}

bool rollcall_ledger_note_path(rollcall_ledger_t* l, uint32_t rollcall_id, uint32_t responder_addr,
                               bool responder_pinned, uint8_t hop_count,
                               const uint32_t* relay_path) {
    if (l == NULL || !l->active || !l->open)
        return false;
    if (rollcall_id != l->rollcall_id)
        return false;
    if (responder_addr == 0 || responder_addr == l->initiator_addr)
        return false;

    /* Only a pinned address may open a row on the strength of a receipt; see
     * this function's contract in rollcall.h for why an unpinned one may
     * not. An existing row is written either way: a path that arrives for a
     * member already in the ledger costs nothing and tells the operator how
     * the frame travelled. */
    rollcall_entry_t* e =
        responder_pinned ? entry_upsert(l, responder_addr, false) : entry_find(l, responder_addr);
    if (e == NULL)
        return false;

    uint8_t n = hop_count;
    if (n > ROLLCALL_PATH_MAX)
        n = ROLLCALL_PATH_MAX;
    e->hop_count = n;
    memset(e->relay_path, 0, sizeof(e->relay_path));
    if (n > 0 && relay_path != NULL)
        memcpy(e->relay_path, relay_path, (size_t)n * sizeof(uint32_t));
    e->have_path = true;
    return true;
}

const rollcall_entry_t* rollcall_ledger_find(const rollcall_ledger_t* l, uint32_t addr) {
    if (l == NULL || !l->active)
        return NULL;
    for (uint8_t i = 0; i < l->entry_count; i++) {
        if (l->entries[i].used && l->entries[i].addr == addr)
            return &l->entries[i];
    }
    return NULL;
}

uint8_t rollcall_ledger_responded_count(const rollcall_ledger_t* l) {
    if (l == NULL || !l->active)
        return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < l->entry_count; i++) {
        if (l->entries[i].used && l->entries[i].responded)
            n++;
    }
    return n;
}

uint8_t rollcall_ledger_missing(const rollcall_ledger_t* l, uint32_t* out, uint8_t out_cap) {
    if (l == NULL || !l->active || !l->anchored)
        return 0;
    uint8_t total = 0;
    for (uint8_t i = 0; i < l->expected_count; i++) {
        const rollcall_entry_t* e = rollcall_ledger_find(l, l->expected[i]);
        if (e != NULL && e->responded)
            continue;
        if (out != NULL && total < out_cap)
            out[total] = l->expected[i];
        total++;
    }
    return total;
}
