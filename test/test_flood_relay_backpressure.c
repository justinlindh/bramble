/*
 * Flood relay backpressure (issue #87).
 *
 * These are regression tests, not characterization tests. The pre-fix
 * schedule_flood_relay ended with:
 *
 *     ESP_LOGW(TAG, "Flood relay queue full; relaying immediately");
 *     if (mesh_tx(buf, len, tx_kind) == TX_GATE_ERR_BUDGET) { ... }
 *
 * so a full queue produced a TRANSMISSION, un-jittered, on a channel this
 * node had just proven it was congested on. The behaviour under test here
 * is the opposite: a full queue produces a DROP and a counter increment,
 * with no transmit path. Restoring the old branch makes
 * test_full_queue_drops_and_counts and test_full_queue_leaves_every_pending
 * _relay_untouched fail, because a queued frame would have had to be handed
 * to the radio and the drop counter would never move.
 */

#include <string.h>

#include "unity.h"

#include "../components/routing/channel_flood.c"
#include "../components/routing/discovery.c"
#include "../components/routing/routing.c"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t FRAME[] = {0xB0, 0x0B, 0x1E, 0x55, 0x01, 0x02, 0x03, 0x04};
#define FRAME_LEN ((uint8_t)sizeof(FRAME))

static void fill_queue(pending_flood_relay_t* q, int capacity) {
    uint32_t drops = 0;
    for (int i = 0; i < capacity; i++) {
        TEST_ASSERT_TRUE(channel_flood_relay_admit(q, capacity, FRAME, FRAME_LEN, 1000 + i,
                                                   0xF00D0000u + i, 1, &drops));
    }
    TEST_ASSERT_EQUAL_UINT32(0, drops);
}

/* --- The fix: a full queue drops, and the drop is observable. --- */

void test_full_queue_drops_and_counts(void) {
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY] = {0};
    fill_queue(q, FLOOD_RELAY_QUEUE_CAPACITY);

    uint32_t drops = 0;
    TEST_ASSERT_FALSE(channel_flood_relay_admit(q, FLOOD_RELAY_QUEUE_CAPACITY, FRAME, FRAME_LEN,
                                                9999, 0xDEADBEEFu, 1, &drops));
    TEST_ASSERT_EQUAL_UINT32(1, drops);
}

void test_repeated_congestion_accumulates_drops_rather_than_transmitting(void) {
    /* A sustained storm past the queue depth must keep shedding load. Every
     * one of these 200 frames was a transmission under the old behaviour. */
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY] = {0};
    fill_queue(q, FLOOD_RELAY_QUEUE_CAPACITY);

    uint32_t drops = 0;
    for (int i = 0; i < 200; i++) {
        TEST_ASSERT_FALSE(channel_flood_relay_admit(q, FLOOD_RELAY_QUEUE_CAPACITY, FRAME, FRAME_LEN,
                                                    5000, 0xC0FFEE00u + i, 1, &drops));
    }
    TEST_ASSERT_EQUAL_UINT32(200, drops);
}

void test_full_queue_leaves_every_pending_relay_untouched(void) {
    /* A drop must not evict, reorder, or re-time an already-scheduled relay:
     * the frames that DID win a slot keep their jitter. */
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY] = {0};
    fill_queue(q, FLOOD_RELAY_QUEUE_CAPACITY);

    pending_flood_relay_t before[FLOOD_RELAY_QUEUE_CAPACITY];
    memcpy(before, q, sizeof(q));

    uint32_t drops = 0;
    channel_flood_relay_admit(q, FLOOD_RELAY_QUEUE_CAPACITY, FRAME, FRAME_LEN, 4242, 0xABCDEF01u, 1,
                              &drops);

    TEST_ASSERT_EQUAL_MEMORY(before, q, sizeof(q));
    TEST_ASSERT_EQUAL_UINT32(1, drops);
}

void test_drop_counter_may_be_null(void) {
    /* Callers that do not care about the counter must not crash. */
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY] = {0};
    fill_queue(q, FLOOD_RELAY_QUEUE_CAPACITY);

    TEST_ASSERT_FALSE(
        channel_flood_relay_admit(q, FLOOD_RELAY_QUEUE_CAPACITY, FRAME, FRAME_LEN, 1, 2, 1, NULL));
}

/* --- Congestion is transient: freeing a slot restores relaying. --- */

void test_freed_slot_is_reused_and_stops_dropping(void) {
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY] = {0};
    fill_queue(q, FLOOD_RELAY_QUEUE_CAPACITY);

    uint32_t drops = 0;
    TEST_ASSERT_FALSE(channel_flood_relay_admit(q, FLOOD_RELAY_QUEUE_CAPACITY, FRAME, FRAME_LEN, 1,
                                                2, 1, &drops));

    /* The relay queue fired (or suppression cancelled) entry 3. */
    q[3].used = false;

    TEST_ASSERT_TRUE(channel_flood_relay_admit(q, FLOOD_RELAY_QUEUE_CAPACITY, FRAME, FRAME_LEN,
                                               7777, 0x11223344u, 1, &drops));
    TEST_ASSERT_TRUE(q[3].used);
    TEST_ASSERT_EQUAL_UINT32(7777, q[3].due_at_ms);
    TEST_ASSERT_EQUAL_UINT32(0x11223344u, q[3].flood_key);
    TEST_ASSERT_EQUAL_UINT32(1, drops); /* unchanged by the successful admit */
}

/* --- A successful admit stores exactly what the relay queue and the
 * suppression engine need, unchanged from the pre-fix inline code. --- */

void test_successful_admit_records_frame_timing_and_suppression_state(void) {
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY] = {0};
    uint32_t drops = 0;

    TEST_ASSERT_TRUE(channel_flood_relay_admit(q, FLOOD_RELAY_QUEUE_CAPACITY, FRAME, FRAME_LEN,
                                               31337, 0xFEEDFACEu, 9, &drops));

    TEST_ASSERT_TRUE(q[0].used);
    TEST_ASSERT_EQUAL_UINT32(31337, q[0].due_at_ms);
    TEST_ASSERT_EQUAL_UINT8(FRAME_LEN, q[0].len);
    TEST_ASSERT_EQUAL_MEMORY(FRAME, q[0].buf, FRAME_LEN);
    TEST_ASSERT_EQUAL_UINT32(0xFEEDFACEu, q[0].flood_key);
    TEST_ASSERT_EQUAL_UINT8(9, q[0].tx_kind);
    /* heard starts at 0: the copy that triggered this schedule is the FIRST
     * copy, never counted as an overheard one. */
    TEST_ASSERT_EQUAL_UINT8(0, q[0].heard);
    TEST_ASSERT_EQUAL_UINT32(0, drops);
}

void test_admit_fills_the_first_free_slot(void) {
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY] = {0};
    uint32_t drops = 0;
    q[0].used = true;
    q[1].used = true;

    TEST_ASSERT_TRUE(channel_flood_relay_admit(q, FLOOD_RELAY_QUEUE_CAPACITY, FRAME, FRAME_LEN, 5,
                                               6, 1, &drops));

    TEST_ASSERT_TRUE(q[2].used);
    TEST_ASSERT_EQUAL_UINT32(5, q[2].due_at_ms);
}

void test_max_length_frame_is_stored_whole(void) {
    /* len is a uint8_t and buf is BRAMBLE_MAX_PACKET_SIZE, so the largest
     * expressible relay must still fit exactly, with no truncation. */
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY] = {0};
    uint32_t drops = 0;
    uint8_t big[255];
    memset(big, 0xAB, sizeof(big));

    TEST_ASSERT_TRUE(
        channel_flood_relay_admit(q, FLOOD_RELAY_QUEUE_CAPACITY, big, 255, 1, 2, 1, &drops));
    TEST_ASSERT_EQUAL_UINT8(255, q[0].len);
    TEST_ASSERT_EQUAL_MEMORY(big, q[0].buf, 255);
    TEST_ASSERT_EQUAL_UINT32(0, drops);
}

/* --- The dropped relay is genuinely redundant, which is what makes
 * dropping the right call rather than a lossy shortcut: suppression
 * accounting still works on everything that stayed queued. --- */

void test_suppression_still_cancels_a_queued_relay_after_a_drop(void) {
    pending_flood_relay_t q[FLOOD_RELAY_QUEUE_CAPACITY] = {0};
    uint32_t drops = 0;
    TEST_ASSERT_TRUE(channel_flood_relay_admit(q, FLOOD_RELAY_QUEUE_CAPACITY, FRAME, FRAME_LEN, 100,
                                               0x5A5A5A5Au, 1, &drops));
    for (int i = 1; i < FLOOD_RELAY_QUEUE_CAPACITY; i++) {
        TEST_ASSERT_TRUE(channel_flood_relay_admit(q, FLOOD_RELAY_QUEUE_CAPACITY, FRAME, FRAME_LEN,
                                                   100, 0x90000000u + i, 1, &drops));
    }
    TEST_ASSERT_FALSE(channel_flood_relay_admit(q, FLOOD_RELAY_QUEUE_CAPACITY, FRAME, FRAME_LEN,
                                                100, 0x7777u, 1, &drops));

    for (int i = 0; i < FLOOD_SUPPRESS_AFTER; i++) {
        channel_flood_note_overheard(q, FLOOD_RELAY_QUEUE_CAPACITY, 0x5A5A5A5Au);
    }
    TEST_ASSERT_FALSE(q[0].used);
    TEST_ASSERT_EQUAL_UINT32(1, drops);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_full_queue_drops_and_counts);
    RUN_TEST(test_repeated_congestion_accumulates_drops_rather_than_transmitting);
    RUN_TEST(test_full_queue_leaves_every_pending_relay_untouched);
    RUN_TEST(test_drop_counter_may_be_null);
    RUN_TEST(test_freed_slot_is_reused_and_stops_dropping);
    RUN_TEST(test_successful_admit_records_frame_timing_and_suppression_state);
    RUN_TEST(test_admit_fills_the_first_free_slot);
    RUN_TEST(test_max_length_frame_is_stored_whole);
    RUN_TEST(test_suppression_still_cancels_a_queued_relay_after_a_drop);
    return UNITY_END();
}
