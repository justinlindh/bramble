/*
 * SAADC battery backend for the SenseCAP T1000-E: real cell voltage over
 * P0.02/AIN0 through a 2x divider, plus CHRG/VBUS charge detection.
 * Satisfies components/battery/include/battery.h.
 *
 * Board wiring verified 2026-08-01 against Meshtastic's
 * github.com/meshtastic/firmware, variants/nrf52840/tracker-t1000-e/
 * variant.h: BATTERY_PIN 2 (P0.02/AIN0), ADC_MULTIPLIER 2.0F,
 * VBAT_AR_INTERNAL AR_INTERNAL_3_0 / AREF_VOLTAGE 3.0, ADC_RESOLUTION 14,
 * EXT_CHRG_DETECT (32+3)/P1.03, EXT_PWR_DETECT (0+5)/P0.05. The pin MODES
 * (pull-up vs none) are not in variant.h; src/Power.cpp is the consumer and
 * its defaults apply here because neither this variant nor any shared header
 * overrides them: EXT_CHRG_DETECT_MODE and EXT_PWR_DETECT_MODE both default
 * to plain INPUT (no internal pull), and EXT_CHRG_DETECT_VALUE is LOW
 * (charging), EXT_PWR_DETECT_VALUE is HIGH (external power), confirmed by a
 * repo-wide search turning up no EXT_CHRG_DETECT_MODE/EXT_PWR_DETECT_MODE
 * override anywhere. isCharging() for a board with EXT_CHRG_DETECT defined
 * (this one) is `digitalRead(EXT_CHRG_DETECT) == EXT_CHRG_DETECT_VALUE`,
 * i.e. charging = P1.03 low.
 */
#include "battery.h"

#include <string.h>

#include <hal/nrf_gpio.h>
#include <nrfx_saadc.h>

#include "bramble_board.h"
#include "esp_log.h"

static const char* TAG = "battery_saadc";
static bool s_initialized = false;

/*
 * SAADC full-scale for gain 1/5 against the 0.6V internal reference:
 * V_fullscale = V_ref / gain = 0.6V / (1/5) = 3.0V, matching Meshtastic's
 * AR_INTERNAL_3_0 (AREF_VOLTAGE 3.0) for this exact board. A 14-bit oneshot
 * sample (raw in [0, 16383]) maps linearly to [0, 3000) mV at the pin:
 * pin_mv = raw * 3000 / 16384. Integer-only (16383 * 3000 < 5e7, nowhere
 * near uint32_t overflow); the board's 2x divider is applied afterward, on
 * the already-averaged pin_mv, in battery_get_status.
 */
#define SAADC_FULL_SCALE_MV 3000u
#define SAADC_RESOLUTION_COUNTS 16384u /* 2^14, NRF_SAADC_RESOLUTION_14BIT */

/* nrfx >= 3.2.0 typedefs nrf_saadc_value_t to void (buffer pointers are
 * void* now, to stay generic across chips with different native sample
 * widths); the nRF52840's actual result register is 16 bits wide, so the
 * concrete sample type callers declare is int16_t (an int16_t* converts to
 * void* implicitly in C, so it passes straight into nrfx_saadc_buffer_set).
 */
static uint32_t raw_to_pin_mv(int16_t raw) {
    if (raw < 0) /* rail noise near 0V can read a small negative code */
        raw = 0;
    return ((uint32_t)raw * SAADC_FULL_SCALE_MV) / SAADC_RESOLUTION_COUNTS;
}

void battery_init(void) {
    nrfx_saadc_channel_t channel = NRFX_SAADC_DEFAULT_CHANNEL_SE(NRF_SAADC_INPUT_AIN0, 0);
    channel.channel_config.gain = NRF_SAADC_GAIN1_5;
    channel.channel_config.reference = NRF_SAADC_REFERENCE_INTERNAL;

    nrfx_err_t err = nrfx_saadc_init(NRFX_SAADC_DEFAULT_CONFIG_IRQ_PRIORITY);
    if (err != NRFX_SUCCESS) {
        ESP_LOGE(TAG, "nrfx_saadc_init failed: %d", (int)err);
        return;
    }

    err = nrfx_saadc_channel_config(&channel);
    if (err != NRFX_SUCCESS) {
        ESP_LOGE(TAG, "nrfx_saadc_channel_config failed: %d", (int)err);
        return;
    }

    /* Simple mode, NULL event handler: blocking oneshot conversions, no IRQ
     * plumbing needed. nrfx_saadc_mode_trigger() busy-waits for the END
     * event itself (nrfy_saadc_sample_start). */
    err = nrfx_saadc_simple_mode_set(1u << 0, NRF_SAADC_RESOLUTION_14BIT, NRF_SAADC_OVERSAMPLE_DISABLED,
                                      NULL);
    if (err != NRFX_SUCCESS) {
        ESP_LOGE(TAG, "nrfx_saadc_simple_mode_set failed: %d", (int)err);
        return;
    }

    /* CHRG/VBUS detect: plain inputs, no pull (see file header for the
     * Power.cpp citation backing this over the brief's pull-up guess). */
    nrf_gpio_cfg_input(BOARD_PIN_CHRG_DETECT, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(BOARD_PIN_VBUS_DETECT, NRF_GPIO_PIN_NOPULL);

    s_initialized = true;
    ESP_LOGI(TAG, "SAADC battery ADC initialized (AIN0, gain 1/5, 14-bit)");
}

/* One oneshot SAADC conversion at the pin, pre-divider. battery_get_status
 * averages BATTERY_AVG_SAMPLE_COUNT of these through the shared
 * battery_average_mv helper (components/battery/battery_helpers.c), same
 * split as the ESP ADC backend (components/battery/battery.c). */
static uint32_t read_one_sample_mv(void) {
    int16_t raw = 0;
    nrfx_err_t err = nrfx_saadc_buffer_set(&raw, 1);
    if (err != NRFX_SUCCESS) {
        ESP_LOGW(TAG, "saadc buffer_set failed: %d", (int)err);
        return 0;
    }
    err = nrfx_saadc_mode_trigger();
    if (err != NRFX_SUCCESS) {
        ESP_LOGW(TAG, "saadc mode_trigger failed: %d", (int)err);
        return 0;
    }
    return raw_to_pin_mv(raw);
}

void battery_get_status(battery_status_t* out) {
    memset(out, 0, sizeof(*out));

    if (!s_initialized) {
        out->charging = BATTERY_CHG_UNKNOWN;
        return;
    }

    uint32_t samples[BATTERY_AVG_SAMPLE_COUNT];
    for (int i = 0; i < BATTERY_AVG_SAMPLE_COUNT; i++)
        samples[i] = read_one_sample_mv();
    uint32_t pin_mv = battery_average_mv(samples, BATTERY_AVG_SAMPLE_COUNT);

    out->mv = pin_mv * (uint32_t)BOARD_BATTERY_DIVIDER;
    out->pct = battery_mv_to_pct(out->mv);
    out->present = true;

    int chrg_level = nrf_gpio_pin_read(BOARD_PIN_CHRG_DETECT);
    battery_charging_t chrg =
        battery_charging_from_gpio(BOARD_PIN_CHRG_DETECT, 0 /* active LOW */, chrg_level);

    /* VBUS folds into the single "charging" signal rather than staying
     * separate: battery_status_t (Task 5) has no distinct "externally
     * powered but not charging" state, and this mirrors Meshtastic's own
     * isVbusIn(), which falls back to EXT_CHRG_DETECT precisely because
     * EXT_PWR_DETECT alone misses a charge-complete pin. A charger still
     * driving VBUS after CHRG goes inactive is exactly the "plugged in,
     * rail no longer reflects the cell" case battery.h's header comment
     * already documents for the T-Deck, so it gets the same YES/0xFF
     * treatment here. */
    bool vbus_present = nrf_gpio_pin_read(BOARD_PIN_VBUS_DETECT) != 0;
    out->charging = (chrg == BATTERY_CHG_YES || vbus_present) ? BATTERY_CHG_YES : BATTERY_CHG_NO;
}
