/*
 * battery_wrappers: the legacy battery_read_mv/battery_read_pct API,
 * reimplemented as thin wrappers over battery_get_status() so every target
 * implements the read path exactly once (in battery_get_status) and every
 * existing call site keeps working unchanged. Also battery_display_pct(),
 * the convenience one-argument form of battery_helpers.c's
 * battery_display_pct_ema(): it needs a real clock (esp_timer_get_time()),
 * which is why it lives here rather than in battery_helpers.c, which stays
 * free of any ESP-IDF dependency so it can be #included directly into the
 * plain-gcc host test harness (test/test_battery.c).
 */
#include "battery.h"
#include "esp_timer.h"

uint32_t battery_read_mv(void) {
    battery_status_t st;
    battery_get_status(&st);
    return st.mv;
}

uint8_t battery_read_pct(void) {
    battery_status_t st;
    battery_get_status(&st);
    return st.pct;
}

static battery_display_state_t s_display_state;

uint8_t battery_display_pct(uint8_t raw_pct) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return battery_display_pct_ema(&s_display_state, raw_pct, now_ms);
}
