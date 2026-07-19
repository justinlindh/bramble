#include "unity.h"
#include "replay_window.h"
#include "../components/replay_window/replay_window.c"
#include "replay_deferred.h"
#include "../components/replay_window/replay_deferred.c"

static replay_table_t t;
static replay_deferred_t d;
void setUp(void) {
    replay_table_init(&t);
    replay_deferred_init(&d);
}
void tearDown(void) {}

void test_first_counter_accepted(void) {
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 100, 0));
}
void test_exact_replay_rejected(void) {
    replay_check_and_add(&t, 0xAA, 100, 0);
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xAA, 100, 0));
}
void test_forward_shift_accepts(void) {
    replay_check_and_add(&t, 0xAA, 100, 0);
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 200, 0));
}
void test_in_window_reorder_accepts_once(void) {
    replay_check_and_add(&t, 0xAA, 100, 0);
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 90, 0));     /* within 64 */
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xAA, 90, 0)); /* dup */
}
void test_below_window_flagged(void) {
    replay_check_and_add(&t, 0xAA, 1000, 0);
    TEST_ASSERT_EQUAL(REPLAY_BELOW_WINDOW, replay_check_and_add(&t, 0xAA, 100, 0));
}
void test_reboot_higher_counter_accepted(void) {
    /* sender reboots and resumes above its old ceiling: a big jump forward is fine */
    replay_check_and_add(&t, 0xAA, 500, 0);
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 70000, 0));
}
void test_distinct_senders_independent(void) {
    replay_check_and_add(&t, 0xAA, 100, 0);
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xBB, 100, 0));
}

/* BUG A regression: a fresh slot must be distinguished from "high_water is
 * legitimately 0", or a first-ever counter of 0 (the nonce counter's actual
 * first-boot value, see Task 0.4) is replayable forever since every replay
 * of counter 0 re-hits the "fresh slot" sentinel instead of the dup check. */
void test_first_counter_zero_then_replay_rejected(void) {
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 0, 0));
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xAA, 0, 0));
}

/* BUG B regression: an exact 64-counter forward jump must still remember the
 * old high_water at window bit 63; a naive `shift >= 64 -> window = 0` wipes
 * it and lets the old high_water be replayed. */
void test_exact_64_jump_then_replay_old_high_water_rejected(void) {
    replay_check_and_add(&t, 0xAA, 100, 0);
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 164, 0));     /* +64 exactly */
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xAA, 100, 0)); /* replay old */
}

/* Deferred (tier-2) acceptance: Task 0.6. */
void test_deferred_accepts_fresh_then_dedups(void) {
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_deferred_accept(&d, 0xAA, 5, 1000, 2000, 1));
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_deferred_accept(&d, 0xAA, 5, 1000, 2000, 1));
}
void test_deferred_rejects_expired(void) {
    TEST_ASSERT_NOT_EQUAL(
        REPLAY_ACCEPT, replay_deferred_accept(&d, 0xAA, 5, 1000, 1000 + 90000, 1)); /* > 24h old */
}
void test_deferred_fail_closed_when_timesync_untrusted(void) {
    TEST_ASSERT_NOT_EQUAL(REPLAY_ACCEPT, replay_deferred_accept(&d, 0xAA, 5, 1000, 2000, 0));
}

/* Fix 3 (red-team panel): a CHAT message accepted via tier-1 must also be
 * recorded in the tier-2 deferred cache, or a counter that ages out of the
 * 64-entry tier-1 window is in NEITHER dedup structure: capture a
 * delivered chat packet, wait out the 60s packet_id dedup, advance the
 * window past the captured counter+64 (any later authentic packet from
 * the same sender), re-inject the original -> tier-1 BELOW_WINDOW -> a
 * tier-2 cache that was never told about the original acceptance treats
 * it as fresh and re-delivers it. */
void test_tier1_accept_recorded_in_deferred_prevents_later_replay(void) {
    /* Original delivery: counter 100 accepted via tier-1. */
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 100, 0));
    /* The fix under test: handle_data must also record this in the
     * deferred cache so a later below-window replay of counter 100 is
     * caught even after it ages out of the tier-1 window. */
    replay_deferred_mark_seen(&d, 0xAA, 100, 1000);

    /* Sender advances past counter 100 + 64: a later authentic message
     * pushes the tier-1 window forward so counter 100 now reads
     * BELOW_WINDOW. */
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 200, 0));
    TEST_ASSERT_EQUAL(REPLAY_BELOW_WINDOW, replay_check_and_add(&t, 0xAA, 100, 0));

    /* Attacker replays the captured original (counter 100). Tier-2 must
     * reject it as a duplicate, not re-deliver it as if it were a
     * legitimate deferred (store-and-forward) message. */
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_deferred_accept(&d, 0xAA, 100, 1000, 2000, 1));
}

/* Without the mark_seen recording, the same replay sequence is (wrongly)
 * accepted: proves mark_seen is load-bearing, not redundant with tier-1's
 * own dedup, and documents exactly what a future refactor must not drop. */
void test_without_deferred_recording_replay_is_wrongly_accepted(void) {
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 100, 0));
    /* Deliberately NOT calling replay_deferred_mark_seen here. */
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 200, 0));
    TEST_ASSERT_EQUAL(REPLAY_BELOW_WINDOW, replay_check_and_add(&t, 0xAA, 100, 0));
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, /* the gap: re-delivered */
                      replay_deferred_accept(&d, 0xAA, 100, 1000, 2000, 1));
}

/* ── Issue #72: the window must survive a reboot ────────────────────── */

/* THE regression test for #72. Sender-side nonce counters are durable
 * (nonce_counter's reserve-ahead ceiling), so before this fix a node that
 * rebooted came back with every high_water at 0 and re-accepted a batch
 * captured off the air before the reboot, replayed in ascending order. OTA
 * makes the reboot attacker-triggerable. */
void test_counter_accepted_before_restart_is_rejected_after(void) {
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 5000, 0));

    uint8_t blob[REPLAY_TABLE_BLOB_MAX];
    int n = replay_table_serialize(&t, blob, sizeof(blob));
    TEST_ASSERT_GREATER_THAN(0, n);

    /* Simulated reboot: the table is RAM, so it is gone. */
    replay_table_init(&t);
    TEST_ASSERT_EQUAL(0, replay_table_deserialize(&t, blob, (size_t)n, 0));

    /* The captured packet is replayed. It must not be accepted again. */
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xAA, 5000, 0));
    /* Genuinely new traffic from the same sender still flows. */
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 5001, 0));
}

/* Proves the test above is a genuine regression test and not tautological:
 * skip the restore step (which is exactly what the unfixed code did, since
 * there was no persistence at all) and the replay sails through. */
void test_without_restore_replay_after_restart_is_wrongly_accepted(void) {
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 5000, 0));
    replay_table_init(&t); /* reboot, nothing restored: pre-fix behaviour */
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 5000, 0));
}

/* The 64-bit bitmap is not persisted, so a restore must assume the whole
 * band below high_water was already delivered. Fail closed: replaying
 * anything inside the band is a dup, not an in-window reorder. */
void test_restore_fails_closed_across_the_below_window_band(void) {
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 5000, 0));
    uint8_t blob[REPLAY_TABLE_BLOB_MAX];
    int n = replay_table_serialize(&t, blob, sizeof(blob));
    replay_table_init(&t);
    TEST_ASSERT_EQUAL(0, replay_table_deserialize(&t, blob, (size_t)n, 0));

    for (uint64_t d = 1; d <= 64; d++)
        TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xAA, 5000 - d, 0));
    /* Below the band is still BELOW_WINDOW, so tier-2's sent_at check can
     * still rescue a genuinely delayed chat message. */
    TEST_ASSERT_EQUAL(REPLAY_BELOW_WINDOW, replay_check_and_add(&t, 0xAA, 4900, 0));
}

void test_serialize_round_trips_multiple_senders(void) {
    replay_check_and_add(&t, 0xAA, 10, 0);
    replay_check_and_add(&t, 0xBB, 777, 0);
    replay_check_and_add(&t, 0xCC, 0, 0);
    uint8_t blob[REPLAY_TABLE_BLOB_MAX];
    int n = replay_table_serialize(&t, blob, sizeof(blob));
    replay_table_init(&t);
    TEST_ASSERT_EQUAL(0, replay_table_deserialize(&t, blob, (size_t)n, 0));
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xAA, 10, 0));
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xBB, 777, 0));
    /* counter 0 is a legitimate value, not an unset sentinel (BUG A). */
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, 0xCC, 0, 0));
    /* An unrelated sender is unaffected. */
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xDD, 1, 0));
}

/* A torn or tampered blob must be DETECTED, not loaded as a table of bogus
 * high-water marks (which would either brick delivery or, if the bogus
 * values were low, silently disable protection). */
void test_corrupt_blob_is_rejected_and_leaves_table_empty(void) {
    replay_check_and_add(&t, 0xAA, 5000, 0);
    uint8_t blob[REPLAY_TABLE_BLOB_MAX];
    int n = replay_table_serialize(&t, blob, sizeof(blob));

    for (int flip = 0; flip < n; flip++) {
        uint8_t corrupt[REPLAY_TABLE_BLOB_MAX];
        memcpy(corrupt, blob, (size_t)n);
        corrupt[flip] ^= 0x40;
        replay_table_init(&t);
        TEST_ASSERT_EQUAL(-1, replay_table_deserialize(&t, corrupt, (size_t)n, 0));
        /* Rejected, and left initialized/empty rather than half-populated. */
        for (int i = 0; i < REPLAY_MAX_SENDERS; i++)
            TEST_ASSERT_EQUAL(0, t.slots[i].used);
    }
}

void test_truncated_and_malformed_blobs_are_rejected(void) {
    replay_check_and_add(&t, 0xAA, 5000, 0);
    uint8_t blob[REPLAY_TABLE_BLOB_MAX];
    int n = replay_table_serialize(&t, blob, sizeof(blob));

    for (int len = 0; len < n; len++)
        TEST_ASSERT_EQUAL(-1, replay_table_deserialize(&t, blob, (size_t)len, 0));

    uint8_t bad[REPLAY_TABLE_BLOB_MAX];
    memcpy(bad, blob, (size_t)n);
    bad[0] = 0xFE; /* wrong format version */
    TEST_ASSERT_EQUAL(-1, replay_table_deserialize(&t, bad, (size_t)n, 0));

    memcpy(bad, blob, (size_t)n);
    bad[1] = REPLAY_MAX_SENDERS + 1; /* count out of range */
    TEST_ASSERT_EQUAL(-1, replay_table_deserialize(&t, bad, (size_t)n, 0));
}

void test_empty_table_round_trips(void) {
    uint8_t blob[REPLAY_TABLE_BLOB_MAX];
    int n = replay_table_serialize(&t, blob, sizeof(blob));
    TEST_ASSERT_EQUAL(8, n); /* header + CRC, no records */
    TEST_ASSERT_EQUAL(0, replay_table_deserialize(&t, blob, (size_t)n, 0));
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xAA, 1, 0));
}

/* The dirty flag is what keeps flash wear bounded: an idle table must not
 * cause a write, and any high-water advance must cause one. */
void test_dirty_flag_gates_the_flush(void) {
    TEST_ASSERT_FALSE(replay_table_is_dirty(&t));
    replay_check_and_add(&t, 0xAA, 100, 0);
    TEST_ASSERT_TRUE(replay_table_is_dirty(&t));
    replay_table_mark_clean(&t);
    /* A rejected replay changes no persisted state. */
    replay_check_and_add(&t, 0xAA, 100, 0);
    TEST_ASSERT_FALSE(replay_table_is_dirty(&t));
    /* A forward advance does. */
    replay_check_and_add(&t, 0xAA, 101, 0);
    TEST_ASSERT_TRUE(replay_table_is_dirty(&t));
}

/* ── Issue #88: eviction must not reopen the window ─────────────────── */

static void fill_all_slots(uint32_t now_ms) {
    for (uint32_t i = 0; i < REPLAY_MAX_SENDERS; i++)
        TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0x1000 + i, 1000, now_ms));
}

/* THE regression test for #88. src_addr is spoofable, so before this fix an
 * attacker could hammer the table with invented source addresses, force the
 * LRU eviction of a target's slot (resetting its high_water to 0) and then
 * replay anything ever captured from that target. */
void test_eviction_flood_cannot_reopen_window_for_active_sender(void) {
    fill_all_slots(0);
    const uint32_t victim = 0x1000; /* one of the occupants, still active */
    replay_check_and_add(&t, victim, 2000, 100);

    /* Attacker floods spoofed sources while every real sender is recent. */
    for (uint32_t i = 0; i < 500; i++) {
        TEST_ASSERT_EQUAL(REPLAY_REJECT_NO_SLOT, replay_check_and_add(&t, 0xDEAD0000 + i, 1, 200));
    }
    TEST_ASSERT_EQUAL(500, t.evict_denied);
    TEST_ASSERT_EQUAL(0, t.evictions);

    /* The victim's high-water mark is intact, so the replay still fails. */
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_check_and_add(&t, victim, 2000, 300));
}

/* Faithful replica of the PRE-FIX slot_for: LRU with no idle requirement,
 * i.e. every occupied slot is evictable on demand. Kept here so the test
 * below documents exactly what the old policy did and cannot silently start
 * passing for the wrong reason. */
static replay_slot_t* legacy_slot_for(replay_table_t* tab, uint32_t src, uint32_t now) {
    replay_slot_t* lru = &tab->slots[0];
    for (int i = 0; i < REPLAY_MAX_SENDERS; i++) {
        replay_slot_t* s = &tab->slots[i];
        if (s->used && s->src_addr == src)
            return s;
        if (!s->used) {
            s->used = 1;
            s->src_addr = src;
            s->high_water = 0;
            s->window = 0;
            s->seen = 0;
            s->last_seen_ms = now;
            return s;
        }
        if (s->last_seen_ms < lru->last_seen_ms)
            lru = s;
    }
    lru->src_addr = src;
    lru->high_water = 0;
    lru->window = 0;
    lru->seen = 0;
    lru->last_seen_ms = now;
    return lru;
}

/* Proves the test above is a genuine regression test and not vacuous: under
 * the pre-fix allocator, REPLAY_MAX_SENDERS cheap packets with spoofed
 * source addresses cycle the whole table, the victim included. The victim
 * then holds a slot with seen == 0, and replay_window.c's fresh-slot branch
 * accepts ANY counter on such a slot: the window is fully reopened. */
void test_unconditional_lru_eviction_would_reopen_the_window(void) {
    fill_all_slots(0);
    const uint32_t victim = 0x1000;
    replay_check_and_add(&t, victim, 2000, 100);

    for (uint32_t i = 0; i < REPLAY_MAX_SENDERS; i++)
        legacy_slot_for(&t, 0xDEAD0000 + i, 200);

    /* The victim's high-water mark is gone from the table entirely. */
    for (int i = 0; i < REPLAY_MAX_SENDERS; i++)
        TEST_ASSERT_NOT_EQUAL(victim, t.slots[i].src_addr);

    /* Re-admitting the victim under the old policy yields a fresh slot, and
     * a fresh slot accepts whatever counter arrives next, including a
     * captured replay of 2000. */
    replay_slot_t* s = legacy_slot_for(&t, victim, 300);
    TEST_ASSERT_EQUAL(victim, s->src_addr);
    TEST_ASSERT_EQUAL(0, s->seen);
    TEST_ASSERT_EQUAL(0, s->high_water);
}

/* Eviction is still possible, just not on demand: once a slot has genuinely
 * gone quiet for REPLAY_EVICT_MIN_IDLE_MS it is fair game, so the table does
 * not permanently lock out new senders. */
void test_idle_slot_is_evictable_and_counted(void) {
    fill_all_slots(0);
    uint32_t later = REPLAY_EVICT_MIN_IDLE_MS + 1;
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_check_and_add(&t, 0xBEEF, 1, later));
    TEST_ASSERT_EQUAL(1, t.evictions);
    TEST_ASSERT_EQUAL(0, t.evict_denied);
}

/* Deferred (tier-2) table: same attack, same fix. A record inside the 24h
 * TTL is still load-bearing and must not be flooded out. */
void test_deferred_eviction_flood_cannot_reopen_window(void) {
    const uint32_t base = 1000000;
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_deferred_accept(&d, 0xAA, 100, base, base, 1));

    for (uint32_t i = 0; i < 1000; i++) {
        int r = replay_deferred_accept(&d, 0xDEAD0000 + i, i, base, base, 1);
        TEST_ASSERT_NOT_EQUAL(REPLAY_REJECT_DUP, r);
    }
    TEST_ASSERT_TRUE(d.evict_denied > 0);
    TEST_ASSERT_EQUAL(0, d.evictions);

    /* The original record survived, so its replay is still a dup. */
    TEST_ASSERT_EQUAL(REPLAY_REJECT_DUP, replay_deferred_accept(&d, 0xAA, 100, base, base, 1));
}

/* A record older than the TTL can no longer protect anything (the sent_at
 * check rejects that message anyway), so it IS free to recycle. */
void test_deferred_evicts_records_past_the_ttl(void) {
    const uint32_t base = 1000000;
    for (uint32_t i = 0; i < REPLAY_DEFERRED_MAX; i++)
        TEST_ASSERT_EQUAL(REPLAY_ACCEPT, replay_deferred_accept(&d, i, i, base, base, 1));
    uint32_t much_later = base + DEFERRED_TTL_S + 10;
    TEST_ASSERT_EQUAL(REPLAY_ACCEPT,
                      replay_deferred_accept(&d, 0xBEEF, 1, much_later, much_later, 1));
    TEST_ASSERT_EQUAL(1, d.evictions);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_counter_accepted);
    RUN_TEST(test_exact_replay_rejected);
    RUN_TEST(test_forward_shift_accepts);
    RUN_TEST(test_in_window_reorder_accepts_once);
    RUN_TEST(test_below_window_flagged);
    RUN_TEST(test_reboot_higher_counter_accepted);
    RUN_TEST(test_distinct_senders_independent);
    RUN_TEST(test_first_counter_zero_then_replay_rejected);
    RUN_TEST(test_exact_64_jump_then_replay_old_high_water_rejected);
    RUN_TEST(test_deferred_accepts_fresh_then_dedups);
    RUN_TEST(test_deferred_rejects_expired);
    RUN_TEST(test_deferred_fail_closed_when_timesync_untrusted);
    RUN_TEST(test_tier1_accept_recorded_in_deferred_prevents_later_replay);
    RUN_TEST(test_without_deferred_recording_replay_is_wrongly_accepted);
    RUN_TEST(test_counter_accepted_before_restart_is_rejected_after);
    RUN_TEST(test_without_restore_replay_after_restart_is_wrongly_accepted);
    RUN_TEST(test_restore_fails_closed_across_the_below_window_band);
    RUN_TEST(test_serialize_round_trips_multiple_senders);
    RUN_TEST(test_corrupt_blob_is_rejected_and_leaves_table_empty);
    RUN_TEST(test_truncated_and_malformed_blobs_are_rejected);
    RUN_TEST(test_empty_table_round_trips);
    RUN_TEST(test_dirty_flag_gates_the_flush);
    RUN_TEST(test_eviction_flood_cannot_reopen_window_for_active_sender);
    RUN_TEST(test_unconditional_lru_eviction_would_reopen_the_window);
    RUN_TEST(test_idle_slot_is_evictable_and_counted);
    RUN_TEST(test_deferred_eviction_flood_cannot_reopen_window);
    RUN_TEST(test_deferred_evicts_records_past_the_ttl);
    return UNITY_END();
}
