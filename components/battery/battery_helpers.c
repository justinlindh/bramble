/*
 * battery_helpers: pure functions shared by every battery_get_status()
 * implementation (ESP ADC, SAADC, emulator virtual battery, nRF null
 * stub). No hardware access, no ESP-IDF dependency: host-testable in
 * isolation (see test/test_battery.c).
 */
#include "battery.h"

uint32_t battery_average_mv(const uint32_t* samples, const bool* valid, size_t count) {
    if (!samples || count == 0)
        return 0;

    uint64_t sum = 0;
    size_t valid_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (valid && !valid[i])
            continue;
        sum += samples[i];
        valid_count++;
    }
    if (valid_count == 0)
        return 0;
    return (uint32_t)(sum / valid_count);
}

battery_charging_t battery_charging_from_gpio(int chrg_gpio, int chrg_active_level, int level) {
    if (chrg_gpio < 0)
        return BATTERY_CHG_UNKNOWN;
    return (level == chrg_active_level) ? BATTERY_CHG_YES : BATTERY_CHG_NO;
}

uint8_t battery_beacon_pct(battery_charging_t charging, uint8_t pct, bool present) {
    return (charging == BATTERY_CHG_YES || !present) ? 0xFF : pct;
}

uint8_t battery_display_pct_ema(battery_display_state_t* state, uint8_t raw_pct, uint32_t now_ms) {
    if (!state)
        return raw_pct;

    /* Unsigned subtraction: correct across a monotonic clock's wraparound
     * (the same modular-arithmetic idiom used everywhere else in this tree
     * that diffs ms ticks), not just when now_ms happens to be larger. */
    bool long_gap = state->has_value &&
                    (uint32_t)(now_ms - state->last_call_ms) > BATTERY_DISPLAY_SNAP_INTERVAL_MS;
    state->last_call_ms = now_ms;

    if (!state->has_value || long_gap) {
        state->displayed = raw_pct;
        state->has_value = true;
        return state->displayed;
    }

    /* Floor: a genuinely low reading always shows immediately, never
     * smoothed in from above. Without this, a node that actually crossed
     * into danger territory could keep displaying a comfortable-looking
     * percentage for several ticks while the EMA caught up. */
    if (raw_pct <= BATTERY_DANGER_PCT) {
        state->displayed = raw_pct;
        return state->displayed;
    }

    int32_t delta = (int32_t)raw_pct - (int32_t)state->displayed;
    int32_t step = (delta * BATTERY_DISPLAY_EMA_NUM) / BATTERY_DISPLAY_EMA_DEN;
    /* Integer division can round a small nonzero delta down to a zero step,
     * which would stall the display short of the real value forever;
     * guarantee at least 1 unit of progress per call whenever delta != 0. */
    if (step == 0 && delta != 0)
        step = (delta > 0) ? 1 : -1;
    if (step > BATTERY_DISPLAY_STEP_LIMIT)
        step = BATTERY_DISPLAY_STEP_LIMIT;
    if (step < -BATTERY_DISPLAY_STEP_LIMIT)
        step = -BATTERY_DISPLAY_STEP_LIMIT;

    state->displayed = (uint8_t)((int32_t)state->displayed + step);
    return state->displayed;
}
