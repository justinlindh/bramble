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

#include <FreeRTOS.h>
#include <semphr.h>

#include <hal/nrf_gpio.h>
#include <nrfx_saadc.h>

#include "bramble_board.h"
#include "esp_log.h"

static const char* TAG = "battery_saadc";
static bool s_initialized = false;

/* nrfx_saadc has a single global control block (m_cb in nrfx_saadc.c): two
 * tasks each running their own buffer_set/mode_trigger pair race on it.
 * On this build that is not hypothetical, mesh_beacon.c's beacon tick (mesh
 * task) and rpc_methods.c's getStatus/getBattery handlers (ble_rpc task,
 * see components/ble/ble_server.c's ble_rpc_task) both call
 * battery_get_status(). An interleaving where task B's buffer_set overwrites
 * task A's mid-conversion m_cb.buffer_primary (SIMPLE_MODE accepts this
 * silently) leaves A spinning on an END event B's trigger already consumed,
 * which hangs the mesh task forever with the SAADC left disabled. Meshtastic
 * hits the same shared-peripheral problem and solves it the same way (a
 * lock around every SAADC access, nrf52SaadcLock in Power.cpp). Static
 * allocation, same idiom as nrf/shim/console_uart.c and
 * nrf/shim/esp_random_nrf.c. */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

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

/* The nRF52840's eight SAADC analog inputs are wired to a fixed set of GPIO
 * pins (nRF52840 Product Specification): AIN0-AIN3 = P0.02-P0.05, AIN4-AIN7
 * = P0.28-P0.31. It is not a formula nrfx exposes, so it is spelled out here
 * once and keyed off BOARD_PIN_VBAT_ADC (nrf/boards/t1000e.h) instead of a
 * second hardcoded AIN0, so the board header stays the single source of
 * truth for which pin this is. Meshtastic's variant.h comment ("P0.02/AIN0,
 * BAT_ADC") is the data point for this board specifically; the other seven
 * mappings are included so a future board's pin choice fails to compile
 * instead of silently sampling the wrong input. */
#if BOARD_PIN_VBAT_ADC == 2
#define BATTERY_SAADC_AIN NRF_SAADC_INPUT_AIN0
#elif BOARD_PIN_VBAT_ADC == 3
#define BATTERY_SAADC_AIN NRF_SAADC_INPUT_AIN1
#elif BOARD_PIN_VBAT_ADC == 4
#define BATTERY_SAADC_AIN NRF_SAADC_INPUT_AIN2
#elif BOARD_PIN_VBAT_ADC == 5
#define BATTERY_SAADC_AIN NRF_SAADC_INPUT_AIN3
#elif BOARD_PIN_VBAT_ADC == 28
#define BATTERY_SAADC_AIN NRF_SAADC_INPUT_AIN4
#elif BOARD_PIN_VBAT_ADC == 29
#define BATTERY_SAADC_AIN NRF_SAADC_INPUT_AIN5
#elif BOARD_PIN_VBAT_ADC == 30
#define BATTERY_SAADC_AIN NRF_SAADC_INPUT_AIN6
#elif BOARD_PIN_VBAT_ADC == 31
#define BATTERY_SAADC_AIN NRF_SAADC_INPUT_AIN7
#else
#error "BOARD_PIN_VBAT_ADC is not one of the nRF52840's eight SAADC-capable pins"
#endif

void battery_init(void) {
    nrfx_saadc_channel_t channel = NRFX_SAADC_DEFAULT_CHANNEL_SE(BATTERY_SAADC_AIN, 0);
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
    err = nrfx_saadc_simple_mode_set(1u << 0, NRF_SAADC_RESOLUTION_14BIT,
                                     NRF_SAADC_OVERSAMPLE_DISABLED, NULL);
    if (err != NRFX_SUCCESS) {
        ESP_LOGE(TAG, "nrfx_saadc_simple_mode_set failed: %d", (int)err);
        return;
    }

    /* One-time offset calibration: an uncalibrated SAADC offset is a few
     * LSB, and the board's 2x divider doubles whatever error that is before
     * it reaches out->mv. Nordic recommends recalibrating across
     * temperature; a tracker sees real temperature swings, but a periodic
     * recal is Task 8/bench-validation scope (needs a call site and cadence
     * decision against real drift data), so this is the one-shot at boot.
     * Blocking (NULL handler), saves and restores the driver's mode state
     * internally, safe to call after simple_mode_set. */
    err = nrfx_saadc_offset_calibrate(NULL);
    if (err != NRFX_SUCCESS) {
        ESP_LOGW(TAG, "nrfx_saadc_offset_calibrate failed: %d (continuing uncalibrated)", (int)err);
    }

    /* CHRG/VBUS detect: plain inputs, no pull (see file header for the
     * Power.cpp citation backing this over the brief's pull-up guess). */
    nrf_gpio_cfg_input(BOARD_PIN_CHRG_DETECT, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(BOARD_PIN_VBUS_DETECT, NRF_GPIO_PIN_NOPULL);

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);

    s_initialized = true;
    ESP_LOGI(TAG, "SAADC battery ADC initialized (pin %d, gain 1/5, 14-bit)", BOARD_PIN_VBAT_ADC);
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

    /* The whole 8-sample loop runs under the lock: nrfx_saadc's control
     * block is a single global (m_cb in nrfx_saadc.c), and letting another
     * task's buffer_set/mode_trigger pair interleave mid-average would both
     * corrupt this sample set and risk the hang described where s_lock is
     * declared. GPIO reads below don't touch that shared state, so they
     * stay outside the lock. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t samples[BATTERY_AVG_SAMPLE_COUNT];
    for (int i = 0; i < BATTERY_AVG_SAMPLE_COUNT; i++)
        samples[i] = read_one_sample_mv();
    xSemaphoreGive(s_lock);
    uint32_t pin_mv = battery_average_mv(samples, BATTERY_AVG_SAMPLE_COUNT);

    out->mv = pin_mv * (uint32_t)BOARD_BATTERY_DIVIDER;
    out->pct = battery_mv_to_pct(out->mv);
    out->present = true;

    int chrg_level = nrf_gpio_pin_read(BOARD_PIN_CHRG_DETECT);
    battery_charging_t chrg =
        battery_charging_from_gpio(BOARD_PIN_CHRG_DETECT, 0 /* active LOW */, chrg_level);

    /* VBUS folds into the single "charging" signal rather than staying
     * separate: battery_status_t (Task 5) has no distinct "externally
     * powered but not charging" state. This is not a gap Meshtastic's
     * firmware avoids either: its isVbusIn() (src/Power.cpp) is exactly
     * this same OR, VBUS-high or CHRG-active, for precisely the reason
     * this code needs it, EXT_PWR_DETECT alone misses a charge-complete
     * pin that de-asserts CHRG while a charger is still connected.
     * Meshtastic's CHRG-only signal is the separate isCharging(); our
     * single `charging` field plays the role of its isVbusIn(), "a
     * charger is driving the rail right now", not strictly "current is
     * flowing into the cell". A charger still driving VBUS after CHRG
     * goes inactive is exactly the "plugged in, rail no longer reflects
     * the cell" case battery.h's header comment already documents for the
     * T-Deck, so it gets the same YES/0xFF treatment here. */
    bool vbus_present = nrf_gpio_pin_read(BOARD_PIN_VBUS_DETECT) != 0;
    out->charging = (chrg == BATTERY_CHG_YES || vbus_present) ? BATTERY_CHG_YES : BATTERY_CHG_NO;
}
