/* test_alerts.c: the pager alert pattern (components/indicators/alerts.c).
 *
 * Whitebox: compiles alerts.c directly against fake indicator_* backends that
 * record every call, so the beep/vibra schedule and the unread-blink are
 * asserted as absolute (output, value) transitions, not round-tripped through
 * the module's own state.
 */

#include <stdbool.h>
#include <stdint.h>

#include "unity.h"

/* ---- fake indicators backend (records calls) --------------------------- */

#define MAX_CALLS 128
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

/* Extract only buzzer+vibra transitions (the LED blink is tested separately). */
static int bv_seq(call_t* out) {
    int n = 0;
    for (int i = 0; i < s_ncalls; i++)
        if (s_calls[i].what == 'b' || s_calls[i].what == 'v')
            out[n++] = s_calls[i];
    return n;
}

static int led_on_count(void) {
    int c = 0;
    for (int i = 0; i < s_ncalls; i++)
        if (s_calls[i].what == 'l' && s_calls[i].value == 1)
            c++;
    return c;
}

void setUp(void) { reset(); }
void tearDown(void) {}

/* ---- tests -------------------------------------------------------------- */

static void test_trigger_starts_buzzer_and_vibra_immediately(void) {
    alerts_message_received(1000);
    /* message_received itself only kicks buzzer+vibra (LED is tick-driven). */
    TEST_ASSERT_EQUAL_INT(2, s_ncalls);
    TEST_ASSERT_EQUAL_CHAR('b', s_calls[0].what);
    TEST_ASSERT_EQUAL_UINT32(ALERT_BUZZER_HZ, s_calls[0].value);
    TEST_ASSERT_EQUAL_CHAR('v', s_calls[1].what);
    TEST_ASSERT_EQUAL_UINT32(1, s_calls[1].value);
}

static void test_buzzer_vibra_schedule(void) {
    alerts_message_received(0);
    run_ticks(0, 1000);

    /* Expected buzzer/vibra transitions (LED filtered out):
     *   t=0   buzzer on, vibra on (trigger)
     *   t=200 buzzer off (beep 1 done)
     *   t=350 buzzer on  (beep 2)
     *   t=400 vibra off  (pulse done)
     *   t=550 buzzer off (beep 2 done) */
    const call_t expected[] = {
        {'b', ALERT_BUZZER_HZ}, {'v', 1}, {'b', 0}, {'b', ALERT_BUZZER_HZ}, {'v', 0}, {'b', 0},
    };
    call_t seq[MAX_CALLS];
    int n = bv_seq(seq);
    TEST_ASSERT_EQUAL_INT((int)(sizeof(expected) / sizeof(expected[0])), n);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_CHAR(expected[i].what, seq[i].what);
        TEST_ASSERT_EQUAL_UINT32(expected[i].value, seq[i].value);
    }
}

static void test_buzzer_vibra_end_quiet_and_stay_quiet(void) {
    alerts_message_received(0);
    run_ticks(0, 2000);
    call_t seq[MAX_CALLS];
    int before = bv_seq(seq);
    run_ticks(2050, 10000);
    int after = bv_seq(seq);
    /* No further buzzer/vibra output after the pattern (the LED keeps
     * blinking, but that is a separate channel). */
    TEST_ASSERT_EQUAL_INT(before, after);
    /* The last buzzer/vibra transition is an off. */
    TEST_ASSERT_EQUAL_UINT32(0, seq[after - 1].value);
}

static void test_retrigger_mid_pattern_restarts_buzzer(void) {
    alerts_message_received(0);
    run_ticks(0, 250); /* beep 1 done (buzzer off at 200) */
    call_t seq[MAX_CALLS];
    int before = bv_seq(seq);
    alerts_message_received(300); /* new message mid-gap: restart */
    int after = bv_seq(seq);
    /* Buzzer was off (gap), so restart turns it back on; vibra was still on. */
    TEST_ASSERT_EQUAL_INT(before + 1, after);
    TEST_ASSERT_EQUAL_CHAR('b', seq[after - 1].what);
    TEST_ASSERT_EQUAL_UINT32(ALERT_BUZZER_HZ, seq[after - 1].value);
    run_ticks(300, 1500);
    int fin = bv_seq(seq);
    TEST_ASSERT_EQUAL_UINT32(0, seq[fin - 1].value); /* ends quiet */
}

static void test_led_blinks_until_confirmed(void) {
    alerts_message_received(0);
    /* Over ~7 s unconfirmed, the LED pulses once per 2 s period. */
    run_ticks(0, 7000);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(3, led_on_count()); /* ~t=0,2000,4000,6000 */
    /* Each on is a pulse: an off follows within the period. */
    int on = 0, off = 0;
    for (int i = 0; i < s_ncalls; i++) {
        if (s_calls[i].what != 'l')
            continue;
        if (s_calls[i].value)
            on++;
        else
            off++;
    }
    TEST_ASSERT_TRUE(off >= on - 1);
}

static void test_confirm_stops_the_blink(void) {
    alerts_message_received(0);
    run_ticks(0, 3000);
    alerts_confirm();
    int after_confirm = s_ncalls;
    /* Last LED transition is an off. */
    int last_led = -1;
    for (int i = 0; i < s_ncalls; i++)
        if (s_calls[i].what == 'l')
            last_led = i;
    TEST_ASSERT_TRUE(last_led >= 0);
    TEST_ASSERT_EQUAL_UINT32(0, s_calls[last_led].value);
    /* No further LED activity after confirm. */
    run_ticks(3050, 12000);
    for (int i = after_confirm; i < s_ncalls; i++)
        TEST_ASSERT_NOT_EQUAL('l', s_calls[i].what);
}

static void test_unconfirmed_reflects_ack_state(void) {
    TEST_ASSERT_FALSE(alerts_unconfirmed());
    alerts_message_received(0);
    TEST_ASSERT_TRUE(alerts_unconfirmed());
    run_ticks(0, 1000); /* pattern ends; still unconfirmed until a press */
    TEST_ASSERT_TRUE(alerts_unconfirmed());
    alerts_confirm();
    TEST_ASSERT_FALSE(alerts_unconfirmed());
    /* A new message re-arms it. */
    alerts_message_received(2000);
    TEST_ASSERT_TRUE(alerts_unconfirmed());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_trigger_starts_buzzer_and_vibra_immediately);
    RUN_TEST(test_buzzer_vibra_schedule);
    RUN_TEST(test_buzzer_vibra_end_quiet_and_stay_quiet);
    RUN_TEST(test_retrigger_mid_pattern_restarts_buzzer);
    RUN_TEST(test_led_blinks_until_confirmed);
    RUN_TEST(test_confirm_stops_the_blink);
    RUN_TEST(test_unconfirmed_reflects_ack_state);
    return UNITY_END();
}
