/* test_alerts.c: the pager alert pattern (components/indicators/alerts.c).
 *
 * Whitebox: compiles alerts.c directly against fake indicator_* backends that
 * record every call, so the beep/vibra/LED schedule is asserted as absolute
 * (time, output, value) transitions, not round-tripped through the module's
 * own state.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

/* ---- fake indicators backend (records calls) --------------------------- */

#define MAX_CALLS 64
typedef struct {
    char what; /* 'b' buzzer, 'v' vibra, 'l' led */
    uint32_t value;
} call_t;
static call_t s_calls[MAX_CALLS];
static int s_ncalls;

static void record(char what, uint32_t value) {
    if (s_ncalls < MAX_CALLS) {
        s_calls[s_ncalls].what = what;
        s_calls[s_ncalls].value = value;
        s_ncalls++;
    }
}

void indicator_init(void) {}
void indicator_buzzer(uint32_t hz_or_0) { record('b', hz_or_0); }
void indicator_vibra(bool on) { record('v', on ? 1 : 0); }
void indicator_set_led(bool on) { record('l', on ? 1 : 0); }

#include "../components/indicators/alerts.c"

/* ---- helpers ------------------------------------------------------------ */

static void reset(void) {
    s_ncalls = 0;
    alerts_init();
}

/* Tick from `from` to `to` in 50 ms steps, mirroring the main loop. */
static void run_ticks(uint32_t from, uint32_t to) {
    for (uint32_t t = from; t <= to; t += 50)
        alerts_tick(t);
}

void setUp(void) { reset(); }
void tearDown(void) {}

/* ---- tests -------------------------------------------------------------- */

static void test_trigger_starts_buzzer_and_vibra_immediately(void) {
    alerts_message_received(1000);
    TEST_ASSERT_EQUAL_INT(2, s_ncalls);
    TEST_ASSERT_EQUAL_CHAR('b', s_calls[0].what);
    TEST_ASSERT_EQUAL_UINT32(ALERT_BUZZER_HZ, s_calls[0].value);
    TEST_ASSERT_EQUAL_CHAR('v', s_calls[1].what);
    TEST_ASSERT_EQUAL_UINT32(1, s_calls[1].value);
}

static void test_full_pattern_schedule(void) {
    alerts_message_received(0);
    run_ticks(0, 1000);

    /* Expected transitions after the trigger (b:hz, v:1 at t=0):
     *   t=200 buzzer off (beep 1 done)
     *   t=350 buzzer on  (beep 2)
     *   t=400 vibra off  (pulse done)
     *   t=550 buzzer off (beep 2 done)
     * At the 50 ms tick granularity each lands on its exact boundary. */
    const call_t expected[] = {
        {'b', ALERT_BUZZER_HZ}, {'v', 1},               /* trigger */
        {'b', 0},                                        /* 200 */
        {'b', ALERT_BUZZER_HZ},                          /* 350 */
        {'v', 0},                                        /* 400 */
        {'b', 0},                                        /* 550 */
    };
    TEST_ASSERT_EQUAL_INT((int)(sizeof(expected) / sizeof(expected[0])), s_ncalls);
    for (int i = 0; i < s_ncalls; i++) {
        TEST_ASSERT_EQUAL_CHAR(expected[i].what, s_calls[i].what);
        TEST_ASSERT_EQUAL_UINT32(expected[i].value, s_calls[i].value);
    }
}

static void test_pattern_ends_quiet_and_stays_quiet(void) {
    alerts_message_received(0);
    run_ticks(0, 2000);
    int after_pattern = s_ncalls;
    run_ticks(2050, 10000);
    TEST_ASSERT_EQUAL_INT(after_pattern, s_ncalls); /* no further output */
}

static void test_retrigger_mid_pattern_restarts_without_glitch(void) {
    alerts_message_received(0);
    run_ticks(0, 250); /* beep 1 done (buzzer off at 200) */
    int calls_before = s_ncalls;
    alerts_message_received(300); /* new message mid-gap: restart */
    /* Buzzer was off (gap), so restart turns it back on; vibra was still on
     * (400 ms pulse), so no duplicate vibra-on call. */
    TEST_ASSERT_EQUAL_INT(calls_before + 1, s_ncalls);
    TEST_ASSERT_EQUAL_CHAR('b', s_calls[s_ncalls - 1].what);
    TEST_ASSERT_EQUAL_UINT32(ALERT_BUZZER_HZ, s_calls[s_ncalls - 1].value);
    /* The restarted pattern still completes and ends quiet. */
    run_ticks(300, 1500);
    TEST_ASSERT_EQUAL_UINT32(0, s_calls[s_ncalls - 1].value); /* last transition is an off */
}

static void test_led_follows_unread_level(void) {
    alerts_set_unread(true);
    TEST_ASSERT_EQUAL_INT(1, s_ncalls);
    TEST_ASSERT_EQUAL_CHAR('l', s_calls[0].what);
    TEST_ASSERT_EQUAL_UINT32(1, s_calls[0].value);
    alerts_set_unread(true); /* level-driven: no duplicate call */
    TEST_ASSERT_EQUAL_INT(1, s_ncalls);
    alerts_set_unread(false);
    TEST_ASSERT_EQUAL_INT(2, s_ncalls);
    TEST_ASSERT_EQUAL_UINT32(0, s_calls[1].value);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_trigger_starts_buzzer_and_vibra_immediately);
    RUN_TEST(test_full_pattern_schedule);
    RUN_TEST(test_pattern_ends_quiet_and_stays_quiet);
    RUN_TEST(test_retrigger_mid_pattern_restarts_without_glitch);
    RUN_TEST(test_led_follows_unread_level);
    return UNITY_END();
}
