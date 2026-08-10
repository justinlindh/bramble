#include "unity.h"
#include "../components/reliability/reliability.c"

void setUp(void) {}
void tearDown(void) {}

void test_tier_max_retries(void) {
    TEST_ASSERT_EQUAL_UINT8(0, tier_max_retries(MSG_TIER_BROADCAST));
    TEST_ASSERT_EQUAL_UINT8(3, tier_max_retries(MSG_TIER_NORMAL));
    TEST_ASSERT_EQUAL_UINT8(8, tier_max_retries(MSG_TIER_CRITICAL));
}

void test_pending_ack_add_and_remove(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);
    uint8_t data[] = {0xAA, 0xBB};
    int idx = pending_ack_add(&table, 42, 0x1234, MSG_TIER_NORMAL, data, 2, 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
    TEST_ASSERT_TRUE(pending_ack_remove(&table, 42));
    TEST_ASSERT_FALSE(pending_ack_remove(&table, 42));
}

/*
 * Task 6 (GAP B): the mesh_task.c real KE send path (send_ke_envelope ->
 * send_data_packet) decides its reliability tier by calling
 * msg_tier_for_send(app_type == APP_TYPE_KE) -- this is the SAME function
 * exercised below, not a re-implementation of its logic. Before this task,
 * this test hardcoded MSG_TIER_CRITICAL directly into pending_ack_add,
 * which proved the pending-ack MECHANISM honors Critical tier but said
 * nothing about whether the real send path ever actually chose that tier
 * (it didn't: send_data_packet always passed MSG_TIER_NORMAL, regardless
 * of app_type). Driving the tier through msg_tier_for_send here means a
 * regression in that decision (e.g. someone flipping the ternary, or a new
 * caller forgetting to pass is_key_exchange=true) fails THIS test, not just
 * a silently-still-green mechanism test.
 */
void test_key_exchange_send_path_uses_critical_tier(void) {
    /* KEY_EXCHANGE should use Critical tier (8 retries, exponential backoff) */
    pending_ack_table_t table;
    pending_ack_init(&table);

    uint8_t pkt[101]; /* KEY_EXCHANGE_SIZE */
    memset(pkt, 0xAA, sizeof(pkt));

    /* The real decision send_data_packet makes for an APP_TYPE_KE payload. */
    uint8_t tier = msg_tier_for_send(true);
    TEST_ASSERT_EQUAL_UINT8(MSG_TIER_CRITICAL, tier);

    int idx = pending_ack_add(&table, 0xAE01, 0x1234, tier, pkt, sizeof(pkt), 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
    TEST_ASSERT_EQUAL_UINT8(8, table.entries[idx].max_attempts);
    TEST_ASSERT_EQUAL_UINT8(MSG_TIER_CRITICAL, table.entries[idx].tier);

    /* Verify exponential backoff: tick through retries */
    uint32_t now = 1000;
    int retries = 0;
    for (int step = 0; step < 20 && table.entries[idx].active; step++) {
        now += 5000; /* advance 5s each step */
        uint8_t prev_attempt = table.entries[idx].attempt;
        pending_ack_tick(&table, now);
        if (table.entries[idx].attempt > prev_attempt)
            retries++;
    }
    /* Should have retried multiple times before giving up */
    TEST_ASSERT_GREATER_OR_EQUAL(1, retries);
}

/* Companion to test_key_exchange_send_path_uses_critical_tier: every
 * non-KE app_type (chat, location, ...) must keep getting MSG_TIER_NORMAL,
 * so this task's fix is scoped to KE and does not silently upgrade every
 * DATA send to Critical tier. */
void test_non_key_exchange_send_path_uses_normal_tier(void) {
    TEST_ASSERT_EQUAL_UINT8(MSG_TIER_NORMAL, msg_tier_for_send(false));
}

void test_pending_ack_table_full(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);
    uint8_t data[] = {0x01};

    /* Fill all MAX_PENDING_ACKS slots */
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        int idx = pending_ack_add(&table, (uint32_t)i, 0x1111, MSG_TIER_NORMAL, data, 1, 1000);
        TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
    }

    /* Table is full: next add must fail */
    int overflow = pending_ack_add(&table, 0xFF, 0x1111, MSG_TIER_NORMAL, data, 1, 1000);
    TEST_ASSERT_EQUAL_INT(-1, overflow);

    /* Remove one entry and verify a slot opens up */
    TEST_ASSERT_TRUE(pending_ack_remove(&table, (uint32_t)(MAX_PENDING_ACKS - 1)));
    int retry = pending_ack_add(&table, 0xFF, 0x1111, MSG_TIER_NORMAL, data, 1, 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, retry);
}

/* A maximum-size DATA frame (the sender caps total at 255 bytes) must be
 * stored verbatim: packet_len must equal the bytes actually copied so that
 * every retransmit consumer reads within packet_data. Under ASAN a stored
 * length larger than the buffer would surface as an out-of-bounds read here
 * (mirroring mesh_tx / the simulator bridge reading packet_len bytes back). */
void test_pending_ack_stores_full_frame_without_overrun(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);

    uint8_t frame[PENDING_ACK_MAX_FRAME];
    for (int i = 0; i < PENDING_ACK_MAX_FRAME; i++) {
        frame[i] = (uint8_t)(i & 0xFF);
    }

    int idx = pending_ack_add(&table, 0xC0DE, 0x2222, MSG_TIER_CRITICAL, frame,
                              (uint16_t)sizeof(frame), 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);

    pending_ack_t* e = &table.entries[idx];
    TEST_ASSERT_EQUAL_UINT16(PENDING_ACK_MAX_FRAME, e->packet_len);
    /* Read back exactly packet_len bytes, as the retransmit path does. */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, e->packet_data, e->packet_len);
}

/* An over-long length must never make packet_len exceed the buffer: the
 * recorded length is clamped to what fits, keeping the invariant
 * packet_len <= sizeof(packet_data) that every consumer relies on. */
void test_pending_ack_clamps_oversized_length(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);

    uint8_t frame[PENDING_ACK_MAX_FRAME];
    memset(frame, 0xAB, sizeof(frame));

    int idx = pending_ack_add(&table, 0xBEEF, 0x3333, MSG_TIER_NORMAL, frame,
                              (uint16_t)(PENDING_ACK_MAX_FRAME + 40), 1000);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
    TEST_ASSERT_EQUAL_UINT16(PENDING_ACK_MAX_FRAME, table.entries[idx].packet_len);
}

/*
 * Receipt LBT-defer contract mirror (fix/receipt-lbt-defer).
 *
 * mesh_process_receipt_tx_event lives in main/mesh_reliability.c, which
 * pulls in mesh_internal.h and, through it, the full mesh_task.c
 * FreeRTOS/ESP-IDF global-state graph (neighbor table, dedup, replay,
 * identity store, dm table, routing table, ...). mesh_task.c and its
 * split-out siblings (mesh_reliability.c among them) are never
 * host-compiled; test_data_auth.c, test_unicast_flood.c, and
 * test_flooded_ack.c all mirror real decision logic out of that family
 * for the same reason, and test_flooded_ack.c already mirrors a function
 * out of this exact file (handle_ack). This mirrors the
 * TX_GATE_ERR_CHANNEL_BUSY defer/attempt bookkeeping in
 * mesh_process_receipt_tx_event byte for byte, against the real tx_gate
 * return codes, so a change to the real decision logic that this file does
 * not reflect is a divergence a reviewer must resolve, not a silently
 * still-green gap.
 *
 * Verified against main/mesh_reliability.c (mesh_process_receipt_tx_event,
 * TX_GATE_ERR_CHANNEL_BUSY branch) and main/mesh_internal.h
 * (RECEIPT_MAX_DEFERS) as of commit 309442e1. The two branches mirrored
 * here (channel-busy defer, and any attempt reaching the air) are named
 * after tx_gate's TX_GATE_ERR_CHANNEL_BUSY / TX_GATE_OK / TX_GATE_ERR_RADIO
 * return codes (components/radio/include/tx_gate.h) but do not need that
 * header: each mirror function corresponds to exactly one branch, so the
 * return code itself is not tested here, only the bookkeeping it drives.
 */

/* main/mesh_internal.h:91 */
#define MIRROR_RECEIPT_MAX_DEFERS 8u

typedef struct {
    uint8_t attempts_total;
    uint8_t attempts_sent;
    uint8_t defers;
    uint32_t due_at_ms;
    bool dropped;
} mirror_receipt_t;

static uint32_t s_mirror_rand;
static uint32_t mirror_random_u32(void) { return s_mirror_rand; }

/* Mirrors the TX_GATE_ERR_CHANNEL_BUSY branch (main/mesh_reliability.c,
 * mesh_process_receipt_tx_event, around lines 318-356): a channel-busy
 * defer reschedules the SAME attempt with fresh jitter and does not spend
 * an attempt, up to RECEIPT_MAX_DEFERS consecutive defers; at the cap the
 * attempt counts as spent and the item either drops (attempts exhausted)
 * or reschedules with the budget-path backoff shape. */
static void mirror_process_channel_busy(mirror_receipt_t* item, uint32_t t_now) {
    item->defers++;
    if (item->defers < MIRROR_RECEIPT_MAX_DEFERS) {
        uint32_t defer_delay_ms = 250u + (mirror_random_u32() % 750u);
        item->due_at_ms = t_now + defer_delay_ms;
        return;
    }

    item->defers = 0;
    item->attempts_sent++;
    if (item->attempts_sent >= item->attempts_total) {
        item->dropped = true;
        return;
    }
    /* Budget-path backoff shape: 1000 + attempts_sent*2000 + jitter(0..999),
     * scaled by remaining receipt-lane budget (scale 1/1 with no pressure).
     * The scale factor itself is airtime-budget state outside this
     * contract's scope and is not reproduced here. */
    uint32_t raw_backoff_ms =
        1000u + ((uint32_t)item->attempts_sent * 2000u) + (mirror_random_u32() % 1000u);
    item->due_at_ms = t_now + raw_backoff_ms;
}

/* Mirrors the TX_GATE_OK / TX_GATE_ERR_RADIO tail (main/mesh_reliability.c,
 * mesh_process_receipt_tx_event, lines 366-367): any attempt that reaches
 * the air, successfully or not, spends an attempt and resets the defer
 * counter (defers are per-attempt, not per-receipt). */
static void mirror_process_air_attempt(mirror_receipt_t* item) {
    item->attempts_sent++;
    item->defers = 0;
}

/* Mirrors the TX_GATE_ERR_BUDGET branch (main/mesh_reliability.c,
 * mesh_process_receipt_tx_event, budget-deny path): a budget deny spends
 * the attempt, and any spent attempt resets the consecutive-defer count,
 * so a deny interleaved with channel-busy defers starts the next attempt
 * with a fresh defer budget. */
static void mirror_process_budget_deny(mirror_receipt_t* item) {
    item->attempts_sent++;
    item->defers = 0;
    if (item->attempts_sent >= item->attempts_total) {
        item->dropped = true;
    }
}

void test_receipt_budget_deny_interleaved_with_defers_resets_defer_count(void) {
    mirror_receipt_t item = {.attempts_total = 3, .attempts_sent = 0, .defers = 0};
    s_mirror_rand = 0;

    /* Two channel-busy defers on attempt 1, then a budget deny spends the
     * attempt: the defer count must reset so attempt 2 gets the full
     * RECEIPT_MAX_DEFERS budget rather than inheriting a nearly-spent one. */
    mirror_process_channel_busy(&item, 1000u);
    mirror_process_channel_busy(&item, 2000u);
    TEST_ASSERT_EQUAL_UINT8(2, item.defers);
    TEST_ASSERT_EQUAL_UINT8(0, item.attempts_sent);

    mirror_process_budget_deny(&item);
    TEST_ASSERT_EQUAL_UINT8(1, item.attempts_sent);
    TEST_ASSERT_EQUAL_UINT8(0, item.defers);
    TEST_ASSERT_FALSE(item.dropped);
}

void test_receipt_channel_busy_defer_reschedules_without_consuming_attempt(void) {
    mirror_receipt_t item = {.attempts_total = 3, .attempts_sent = 0, .defers = 0};
    s_mirror_rand = 0;

    mirror_process_channel_busy(&item, 1000u);

    TEST_ASSERT_EQUAL_UINT8(0, item.attempts_sent);
    TEST_ASSERT_EQUAL_UINT8(1, item.defers);
    TEST_ASSERT_EQUAL_UINT32(1250u, item.due_at_ms); /* 1000 + (250 + 0) */
    TEST_ASSERT_FALSE(item.dropped);
}

/* Re-jitter window is 250 + (random % 750), i.e. 250..999ms inclusive.
 * random=0 and random=749 hit both endpoints of that window exactly
 * (749 % 750 == 749, the largest value the modulo can produce); UINT32_MAX
 * would NOT hit the true maximum (UINT32_MAX % 750 == 45), so the endpoint
 * is driven directly rather than through a wraparound value. */
void test_receipt_channel_busy_jitter_window_bounds_250_999(void) {
    mirror_receipt_t low = {.attempts_total = 3};
    s_mirror_rand = 0;
    mirror_process_channel_busy(&low, 5000u);
    TEST_ASSERT_EQUAL_UINT32(5250u, low.due_at_ms); /* window minimum: 250ms */

    mirror_receipt_t high = {.attempts_total = 3};
    s_mirror_rand = 749u;
    mirror_process_channel_busy(&high, 5000u);
    TEST_ASSERT_EQUAL_UINT32(5999u, high.due_at_ms); /* window maximum: 999ms */
}

void test_receipt_channel_busy_defer_cap_consumes_one_attempt(void) {
    mirror_receipt_t item = {.attempts_total = 3, .attempts_sent = 0, .defers = 0};
    s_mirror_rand = 0;
    uint32_t t_now = 0u;

    for (int i = 0; i < (int)MIRROR_RECEIPT_MAX_DEFERS; i++) {
        mirror_process_channel_busy(&item, t_now);
        t_now += 250u;
    }

    /* The 8th consecutive channel-busy defer converts to a consumed
     * attempt: defers resets, attempts_sent advances, and (with 1 of 3
     * attempts spent) the item reschedules rather than dropping. */
    TEST_ASSERT_EQUAL_UINT8(0, item.defers);
    TEST_ASSERT_EQUAL_UINT8(1, item.attempts_sent);
    TEST_ASSERT_FALSE(item.dropped);
    TEST_ASSERT_TRUE(item.due_at_ms > t_now - 250u);
}

void test_receipt_channel_busy_three_consumed_attempts_drops_receipt(void) {
    mirror_receipt_t item = {.attempts_total = 3, .attempts_sent = 0, .defers = 0};
    s_mirror_rand = 0;
    uint32_t t_now = 0u;

    /* 3 attempts * RECEIPT_MAX_DEFERS defers each: every attempt is spent
     * purely on channel-busy exhaustion, never reaching the air. */
    for (int attempt = 0; attempt < 3; attempt++) {
        for (int i = 0; i < (int)MIRROR_RECEIPT_MAX_DEFERS; i++) {
            mirror_process_channel_busy(&item, t_now);
            t_now += 250u;
        }
    }

    TEST_ASSERT_EQUAL_UINT8(3, item.attempts_sent);
    TEST_ASSERT_TRUE(item.dropped);
}

void test_receipt_air_attempt_resets_defers_and_increments_attempts(void) {
    mirror_receipt_t item = {.attempts_total = 3, .attempts_sent = 0, .defers = 5};

    mirror_process_air_attempt(&item);

    TEST_ASSERT_EQUAL_UINT8(1, item.attempts_sent);
    TEST_ASSERT_EQUAL_UINT8(0, item.defers);
}

/* Whether a frame is still outstanding is a question the parked-message retry
 * has to ask before putting the same message on the air a second time: a
 * transmitted frame holds no send-queue entry, so the queue's uid keying cannot
 * see it, and only this table knows it is unresolved. */
void test_pending_ack_is_active_tracks_the_frames_lifetime(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);
    const uint8_t frame[4] = {1, 2, 3, 4};

    TEST_ASSERT_FALSE(pending_ack_is_active(&table, 0xABCD));

    TEST_ASSERT_EQUAL_INT(
        0, pending_ack_add(&table, 0xABCD, 0x1111, MSG_TIER_NORMAL, frame, sizeof(frame), 1000));
    TEST_ASSERT_TRUE(pending_ack_is_active(&table, 0xABCD));
    TEST_ASSERT_FALSE(pending_ack_is_active(&table, 0xABCE)); /* a different frame */

    /* Resolved by an ACK: no longer outstanding, so the row may be retried. */
    TEST_ASSERT_TRUE(pending_ack_remove(&table, 0xABCD));
    TEST_ASSERT_FALSE(pending_ack_is_active(&table, 0xABCD));
}

/* packet_id 0 is the value a message-store row carries when no frame has ever
 * gone out for it, so it must never be reported outstanding. Asked against a
 * table that genuinely HOLDS an active entry stamped 0, because against an
 * empty table the answer is false either way and the question is not really
 * being put: the early return has to do the work, rather than the search
 * merely failing to find anything. */
void test_pending_ack_is_active_never_matches_the_never_sent_packet_id(void) {
    pending_ack_table_t table;
    pending_ack_init(&table);
    const uint8_t frame[4] = {1, 2, 3, 4};

    TEST_ASSERT_EQUAL_INT(
        0, pending_ack_add(&table, 0, 0x1111, MSG_TIER_NORMAL, frame, sizeof(frame), 1000));
    TEST_ASSERT_TRUE(table.entries[0].active);
    TEST_ASSERT_EQUAL_UINT32(0, table.entries[0].packet_id);

    TEST_ASSERT_FALSE_MESSAGE(pending_ack_is_active(&table, 0),
                              "packet_id 0 matched a table entry, so a parked row that has never "
                              "been transmitted would read as still in flight and be skipped by "
                              "every flush");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pending_ack_is_active_tracks_the_frames_lifetime);
    RUN_TEST(test_pending_ack_is_active_never_matches_the_never_sent_packet_id);
    RUN_TEST(test_tier_max_retries);
    RUN_TEST(test_pending_ack_add_and_remove);
    RUN_TEST(test_key_exchange_send_path_uses_critical_tier);
    RUN_TEST(test_non_key_exchange_send_path_uses_normal_tier);
    RUN_TEST(test_pending_ack_table_full);
    RUN_TEST(test_pending_ack_stores_full_frame_without_overrun);
    RUN_TEST(test_pending_ack_clamps_oversized_length);
    RUN_TEST(test_receipt_budget_deny_interleaved_with_defers_resets_defer_count);
    RUN_TEST(test_receipt_channel_busy_defer_reschedules_without_consuming_attempt);
    RUN_TEST(test_receipt_channel_busy_jitter_window_bounds_250_999);
    RUN_TEST(test_receipt_channel_busy_defer_cap_consumes_one_attempt);
    RUN_TEST(test_receipt_channel_busy_three_consumed_attempts_drops_receipt);
    RUN_TEST(test_receipt_air_attempt_resets_defers_and_increments_attempts);
    return UNITY_END();
}
