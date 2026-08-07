/*
 * SAADC battery backend for the SenseCAP T1000-E: cell voltage over
 * P0.02/AIN0 through a 2x divider, plus CHRG/VBUS charge detection.
 * Satisfies components/battery/include/battery.h.
 *
 * THE VOLTAGE READING THIS FILE PRODUCES IS WRONG, AND THE BRANCH CARRYING
 * IT IS BLOCKED ON THAT. Read this before changing anything here.
 *
 * The divider on P0.02 hangs off a sensor rail that nothing in this port
 * powers, so the SAADC measures a dead divider. The conversions still
 * SUCCEED and return 1 to 4 mV, which is the dangerous part: there is no
 * error for read_one_sample_mv() to detect and discard, so out->mv lands
 * near zero with out->present true, which is a confident wrong answer
 * rather than an honest missing one. On USB it is masked, since VBUS
 * drives charging YES and battery_beacon_pct() substitutes the unknown
 * sentinel; unplugged, the node beacons a real 0 percent and trips the
 * display's low-battery floor.
 *
 * The rail gate is P1.06, SENSE_POWER_EN per Seeed's vendor SDK
 * (smtc_hal_config.h in Seeed-Tracker-T1000-E-for-LoRaWAN-dev-board), and
 * powering it is what makes the divider readable: driven high at runtime it
 * measured 3920 mV at the pin on a charging cell, and released it returned
 * to 2 mV. Do NOT add that drive here on the strength of that measurement.
 * The same drive from this file's boot-time path stopped the board within
 * about a millisecond: the flash boot trace ends on the stamp written
 * immediately after the pin write, with no failure tag from any
 * instrumented fatal path (assert, nrfx assert, hard fault, stack overflow,
 * malloc failure) and no rescue from the no-advertising sentinel, so it
 * reset or locked up rather than blocking. Why a runtime drive is fine and
 * a boot-path drive was not is NOT established, and neither is brownout
 * versus lockup; separating those needs a build that records a reset reason
 * per boot. The correct implementation of this reading is being built
 * against the runtime measurement rather than bolted onto this file, so
 * this backend does not touch that pin.
 *
 * Charge detection below is unaffected by any of this: it needs neither the
 * ADC nor that rail, both detect pins are plain GPIO inputs, and its
 * verdicts were confirmed on hardware against a charger being plugged and
 * unplugged.
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
 * Bench (T8, physical T1000-E) found the blocking NULL-handler path hangs
 * the mesh task outright: an A/B build with battery_null instead of this
 * backend beacons within 2 minutes, this backend never beacons. The
 * suspect (unproven, do not treat as fact elsewhere) is nrfy_saadc's
 * blocking helpers spinning unbounded on EVENTS_END/EVENTS_CALIBRATEDONE
 * (haly/nrfy_saadc.h): if that spin never resolves, it never will, and
 * there is no way to time out a bare while(!event){} loop from outside it.
 *
 * The fix is to never let nrfx busy-wait on our behalf at all: every SAADC
 * operation runs in event-handler (non-blocking) mode, and this code does
 * its own bounded wait on a binary semaphore that the event handler signals
 * from ISR context. A timeout here is a real, catchable failure (the sample
 * or calibration attempt fails), not a permanent lockup.
 *
 * IRQ priority: nrfx_saadc_init() is given NRFX_SAADC_DEFAULT_CONFIG_IRQ_
 * PRIORITY (7, from nrf/config's nrfx_config_nrf52840.h template). This
 * build's FreeRTOSConfig.h sets configPRIO_BITS=3 and
 * configMAX_SYSCALL_INTERRUPT_PRIORITY=(2 << (8-3)), i.e. priority level 2;
 * FreeRTOS's rule is that any ISR calling a FromISR API must sit at a
 * priority level numerically >= that threshold, so 7 (numerically above 2)
 * is safe. This mirrors the two established idioms already in this port:
 * gps_t1000e.c's UARTE1 handler (default priority, also 7) calls
 * xSemaphoreGiveFromISR/portYIELD_FROM_ISR at that same level, and
 * radio_lr1110.c explicitly documents choosing GPIOTE priority 6 for the
 * same reason ("stays below, numerically above, the FreeRTOS max-syscall
 * priority so the ISR may use the FromISR API"). Getting this backwards
 * (a priority numerically below the threshold, i.e. 0 or 1) would let the
 * ISR run above where FreeRTOS's critical-section masking applies, so a
 * FromISR call from it corrupts kernel state instead of hanging; nothing
 * here overrides the default, so this build is not exposed to that.
 */
static SemaphoreHandle_t s_done;
static StaticSemaphore_t s_done_buf;

/* Generous relative to a real oneshot conversion (tens of microseconds:
 * 10us acquisition plus 14-bit conversion time) or a real calibration
 * (documented as similarly short); if this fires, the peripheral is
 * actually stuck, not just slow, which is exactly the condition the old
 * blocking code could never detect or recover from. */
#define SAADC_WAIT_MS 25

/* Shared by both the per-sample DONE wait and the boot-time calibration
 * wait: nrfx dispatches NRFX_SAADC_EVT_DONE to the handler registered with
 * simple_mode_set and NRFX_SAADC_EVT_CALIBRATEDONE to the one registered
 * with offset_calibrate (nrfx_saadc.c's saadc_event_end_handle). One handler
 * and one semaphore covers both, but NOT because they can be assumed to
 * never overlap in time: a slow calibration can still complete after
 * battery_init has given up waiting on it and moved on to normal sampling.
 * What actually makes sharing safe is that every caller drains s_done to
 * empty immediately before starting a wait it cares about (see
 * read_one_sample_mv and the calibration-timeout cleanup in battery_init),
 * so a late give from something this code has already stopped waiting on
 * can never be mistaken for the completion of a different, later
 * operation. Discovered on physical-hardware bench testing (T8): without
 * that draining, a calibration that completed after its own bounded wait
 * timed out left a stale give that the *first* real sample's wait consumed
 * instantly, before EasyDMA had written anything, handing back a
 * fabricated "valid" 0 mV sample. */
static void saadc_event_handler(nrfx_saadc_evt_t const* p_event) {
    if (p_event->type == NRFX_SAADC_EVT_DONE || p_event->type == NRFX_SAADC_EVT_CALIBRATEDONE) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_done, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

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
    /* Created before anything can wait on it, same ordering discipline as
     * s_lock below: nothing touches the SAADC until s_initialized is true,
     * and both statics exist well before that point. */
    s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);

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

    /* Simple mode, real event handler: every conversion is triggered and
     * completes asynchronously (see the file header for why the NULL,
     * blocking-mode alternative is gone). */
    err = nrfx_saadc_simple_mode_set(1u << 0, NRF_SAADC_RESOLUTION_14BIT,
                                     NRF_SAADC_OVERSAMPLE_DISABLED, saadc_event_handler);
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
     * Evented (the same saadc_event_handler, dispatched on
     * NRFX_SAADC_EVT_CALIBRATEDONE) with a bounded wait, not the blocking
     * NULL-handler path: that path's internal busy-wait is exactly the
     * pattern suspected of hanging sample conversions, so it gets the same
     * treatment here. A calibration that fails to start, or times out,
     * logs a warning and continues uncalibrated: a few LSB of offset error
     * is a correctness nit, not a reason to abandon boot. */
    err = nrfx_saadc_offset_calibrate(saadc_event_handler);
    if (err != NRFX_SUCCESS) {
        ESP_LOGW(TAG, "nrfx_saadc_offset_calibrate failed to start: %d (continuing uncalibrated)",
                 (int)err);
    } else if (xSemaphoreTake(s_done, pdMS_TO_TICKS(SAADC_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "nrfx_saadc_offset_calibrate timed out (continuing uncalibrated)");
        /* Close the door deterministically: without this, a CALIBRATEDONE
         * that arrives after this point gives s_done for nothing waiting on
         * it here, and that stale give survives to falsely satisfy the
         * *first* real sample's wait instead (see saadc_event_handler's
         * comment for the full hazard this caused on bench). Unlike the
         * sample-timeout path below, this does NOT need a follow-up drain:
         * nrfx_saadc_abort() during NRF_SAADC_STATE_CALIBRATION triggers
         * STOP, and nrfx_saadc_irq_handler's STOPPED case explicitly
         * special-cases calibration, if STOP lands before CALIBRATEDONE
         * the calibration is simply abandoned and the event handler is
         * never invoked at all (verified against the vendored
         * nrfx_saadc.c), so no further give is coming to drain. */
        nrfx_saadc_abort();
    }

    /* CHRG/VBUS detect: plain inputs, no pull (see file header for the
     * Power.cpp citation backing this over the brief's pull-up guess). */
    nrf_gpio_cfg_input(BOARD_PIN_CHRG_DETECT, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(BOARD_PIN_VBUS_DETECT, NRF_GPIO_PIN_NOPULL);

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);

    s_initialized = true;
    ESP_LOGI(TAG, "SAADC battery ADC initialized (pin %d, gain 1/5, 14-bit)", BOARD_PIN_VBAT_ADC);
}

/* File-scope, not a stack local: the whole point of read_one_sample_mv's
 * conversion is asynchronous (EasyDMA writes it from an ISR sometime after
 * mode_trigger returns), and a stack slot from a function that has already
 * returned (the timeout path, before this fix) is memory a later write can
 * land in after it has been reused for something else entirely. A single
 * static instance is safe because every access is already serialized by
 * s_lock across the whole averaging loop (battery_get_status), so there is
 * never more than one conversion using it at a time. The timeout path's
 * abort() is kept anyway as belt-and-braces. */
static int16_t s_raw;

/* One oneshot SAADC conversion at the pin, pre-divider. battery_get_status
 * averages BATTERY_AVG_SAMPLE_COUNT of these through the shared
 * battery_average_mv helper (components/battery/battery_helpers.c), same
 * split as the ESP ADC backend (components/battery/battery.c). Reports
 * success via the return value: a failed conversion is excluded from the
 * average (battery_average_mv's valid mask) rather than folded in as a
 * fabricated 0 mV sample, which would corrupt the whole reading toward a
 * false low battery instead of just being one fewer sample in the mean.
 *
 * mode_trigger() returns as soon as the trigger is issued (event-handler
 * mode); the actual conversion completes later, asynchronously, and
 * saadc_event_handler signals s_done from the SAADC ISR when it does. */
static bool read_one_sample_mv(uint32_t* out_mv) {
    /* Drain any stale give before starting this conversion's wait. Under
     * s_lock, with no conversion of ours in flight yet, a pending give
     * here cannot be a real answer: it can only be a leftover from
     * something earlier that this code already stopped waiting on (a late
     * offset-calibration completion, or a previous sample's timeout-abort
     * redelivering its DONE after that sample's own drain gave up). See
     * saadc_event_handler's comment for why leaving it unconsumed is a
     * real bug, not a theoretical one: it was observed on bench. */
    while (xSemaphoreTake(s_done, 0) == pdTRUE) {
    }

    nrfx_err_t err = nrfx_saadc_buffer_set(&s_raw, 1);
    if (err != NRFX_SUCCESS) {
        ESP_LOGW(TAG, "saadc buffer_set failed: %d", (int)err);
        return false;
    }
    err = nrfx_saadc_mode_trigger();
    if (err != NRFX_SUCCESS) {
        ESP_LOGW(TAG, "saadc mode_trigger failed: %d", (int)err);
        return false;
    }
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(SAADC_WAIT_MS)) != pdTRUE) {
        /* The peripheral did not signal completion inside a generous bound:
         * treat it as a failed sample rather than waiting forever (that
         * unbounded wait is the bug this rework removes). Abort the
         * still-possibly-in-flight conversion: belt-and-braces now that
         * s_raw is static rather than a reclaimed stack slot, but aborting
         * is still the right move regardless, since it also stops the
         * peripheral from being left mid-conversion when the next sample's
         * pre-trigger drain and buffer_set run. The abort's own completion
         * is expected to signal s_done a second time (verified: aborting a
         * SIMPLE_MODE_SAMPLE conversion's STOP task also triggers END, per
         * nrfx_saadc_irq_handler); drain that (best-effort, same bound) so
         * it doesn't masquerade as a real result on the *next* sample's
         * wait, on top of that sample's own pre-trigger drain. */
        nrfx_saadc_abort();
        ESP_LOGW(TAG, "saadc sample timed out, aborting");
        (void)xSemaphoreTake(s_done, pdMS_TO_TICKS(SAADC_WAIT_MS));
        return false;
    }
    *out_mv = raw_to_pin_mv(s_raw);
    return true;
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
    bool valid[BATTERY_AVG_SAMPLE_COUNT];
    for (int i = 0; i < BATTERY_AVG_SAMPLE_COUNT; i++)
        valid[i] = read_one_sample_mv(&samples[i]);
    xSemaphoreGive(s_lock);
    uint32_t pin_mv = battery_average_mv(samples, valid, BATTERY_AVG_SAMPLE_COUNT);

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

    /* No-op here (this board's charge/VBUS pins mean the verdict above is
     * already YES or NO, never UNKNOWN), but run through the shared
     * voltage-inference step anyway so every battery_get_status
     * implementation funnels through the same decision. */
    out->charging = battery_infer_charging(out->charging, out->mv);
}
