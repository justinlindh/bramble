/* alerts.c: the pager's message-alert pattern over the indicators interface.
 * Pure timing logic (no OS, no drivers); see alerts.h for the contract. */

#include "alerts.h"

#include "indicators.h"

#include <stddef.h>

/* Pattern step table: offsets from the trigger moment. Buzzer beep-beep and
 * one vibra pulse run concurrently; the pattern is done at ALERT_DONE_MS. */
#define BEEP1_ON_MS 0u
#define BEEP1_OFF_MS (BEEP1_ON_MS + ALERT_BEEP_ON_MS)
#define BEEP2_ON_MS (BEEP1_OFF_MS + ALERT_BEEP_GAP_MS)
#define BEEP2_OFF_MS (BEEP2_ON_MS + ALERT_BEEP_ON_MS)
#define ALERT_DONE_MS (BEEP2_OFF_MS > ALERT_VIBRA_MS ? BEEP2_OFF_MS : ALERT_VIBRA_MS)

static bool s_active;
static uint32_t s_t0;
static bool s_buzzer_on;
static bool s_vibra_on;
static bool s_led_on;

void alerts_init(void) {
    s_active = false;
    s_buzzer_on = false;
    s_vibra_on = false;
    s_led_on = false;
}

void alerts_message_received(uint32_t now_ms) {
    /* Restart the pattern from the top; a burst of messages reads as one
     * continuous alert rather than queueing N patterns back to back. */
    s_active = true;
    s_t0 = now_ms;
    if (!s_buzzer_on) {
        indicator_buzzer(ALERT_BUZZER_HZ);
        s_buzzer_on = true;
    }
    if (!s_vibra_on) {
        indicator_vibra(true);
        s_vibra_on = true;
    }
}

void alerts_set_unread(bool any_unread) {
    if (any_unread != s_led_on) {
        indicator_set_led(any_unread);
        s_led_on = any_unread;
    }
}

void alerts_tick(uint32_t now_ms) {
    if (!s_active)
        return;
    uint32_t at = now_ms - s_t0;

    bool want_buzzer = (at < BEEP1_OFF_MS) || (at >= BEEP2_ON_MS && at < BEEP2_OFF_MS);
    if (want_buzzer != s_buzzer_on) {
        indicator_buzzer(want_buzzer ? ALERT_BUZZER_HZ : 0);
        s_buzzer_on = want_buzzer;
    }

    bool want_vibra = at < ALERT_VIBRA_MS;
    if (want_vibra != s_vibra_on) {
        indicator_vibra(want_vibra);
        s_vibra_on = want_vibra;
    }

    if (at >= ALERT_DONE_MS) {
        /* Pattern complete; make sure both transient outputs are off. */
        if (s_buzzer_on) {
            indicator_buzzer(0);
            s_buzzer_on = false;
        }
        if (s_vibra_on) {
            indicator_vibra(false);
            s_vibra_on = false;
        }
        s_active = false;
    }
}
