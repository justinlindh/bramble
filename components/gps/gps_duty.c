/*
 * GPS duty-cycling policy (Task 9). Pure decision of whether GNSS power
 * should be on right now: rules 1-4 are exact-state shortcuts (user
 * preference, not sharing, interval too short to bother cycling, first
 * share ever), rule 5 is the general case, wake early enough for a warm
 * reacquisition before the next scheduled send. See gps_duty.h for the
 * rule list and the two tunables.
 */
#include "gps_duty.h"

bool gps_duty_should_power(const gps_duty_inputs_t* in) {
    if (!in->user_enabled) {
        return false;
    }
    if (!in->sharing_active) {
        return true;
    }
    if (in->interval_s < GPS_DUTY_MIN_INTERVAL_S) {
        return true;
    }
    if (in->last_send_ms == 0) {
        return true;
    }

    /* Wrap-safe: both operands are uint32_t so the subtraction wraps
     * exactly like the mesh clock does, and the cast to int32_t recovers
     * the correct sign of the (possibly negative, i.e. overdue) time
     * remaining regardless of which side of a wraparound now_ms and
     * last_send_ms fall on. */
    uint32_t next_send_ms = in->last_send_ms + (uint32_t)in->interval_s * 1000u;
    int32_t due_in_ms = (int32_t)(next_send_ms - in->now_ms);
    return due_in_ms <= (int32_t)(GPS_DUTY_WARM_MARGIN_S * 1000);
}
