/*
 * Battery voltage backend for the SenseCAP T1000-E: the cell through the
 * 2x divider on P0.02/AIN0, behind the P1.06 SENSE_POWER_EN gate. Satisfies
 * components/battery/include/battery.h.
 *
 * The divider is dead until the gate is high (see nrf/boards/t1000e.h for
 * the vendor-source and bench evidence), so every read is a gated window:
 * drive P1.06 high, let the divider settle, average a burst of samples,
 * drive it low again. That mirrors the vendor's own sensor_bat_sample()
 * (Seeed-Tracker-T1000-E-for-LoRaWAN-dev-board, sensor.c: rail on,
 * sample, times 2, rail off) with the same push-pull standard-drive
 * active-HIGH configuration its hal_gpio_init_out() uses, and it keeps the
 * rail off the rest of the time, which is both the vendor's power posture
 * and what keeps the NTC/photo dividers on the same rail unpowered.
 *
 * Context discipline: every caller runs on a FreeRTOS task (mesh beacon
 * tick, BLE RPC handlers, and app_init's task_boot snapshot), never before
 * the scheduler, so vTaskDelay for the settle window is safe here. One
 * boot-time read runs from app_init_stack BEFORE BT_BOOT_DONE on purpose:
 * an instrumented build once stopped dead (reset or lockup, undetermined)
 * the first time boot-context code drove P1.06, and placing the first
 * gated window before BT_BOOT_DONE means that if that ever recurs the
 * boot-loop rescue path escapes to DFU with a decodable multi-boot trace
 * instead of looping until the physical gesture. Runtime windows after a
 * surviving boot re-run an action already proven safe live on this unit
 * (2026-08-06 probe: P1.06 high, correct reading, released, no upset).
 *
 * The SAADC never busy-waits on our behalf: every operation runs in
 * event-handler (non-blocking) mode and this code does its own bounded
 * wait on a binary semaphore the event handler gives from ISR context, so
 * a stuck peripheral is a failed read (mv 0 per battery.h's contract),
 * never a hung task. The drain-before-wait discipline exists because a
 * late completion from an operation this code already gave up on (a slow
 * offset calibration, an aborted sample's second DONE) would otherwise
 * satisfy the next wait instantly and hand back a fabricated sample; that
 * failure was observed on bench, not hypothesized.
 *
 * Locking: nrfx_saadc has a single global control block, and two tasks
 * really do race on it in this build (mesh_beacon.c's beacon tick on the
 * mesh task, rpc_methods.c's getStatus/getBattery on the BLE RPC task).
 * One mutex serializes the whole gate-settle-average window.
 *
 * IRQ priority: nrfx_saadc_init takes NRFX_SAADC_DEFAULT_CONFIG_IRQ_
 * PRIORITY (7 from the nrfx template). FreeRTOS requires any ISR using a
 * FromISR API to sit numerically at or above
 * configMAX_SYSCALL_INTERRUPT_PRIORITY (level 2 in this build's
 * FreeRTOSConfig.h), so 7 is safe; same reasoning as gps_t1000e.c's UARTE1
 * handler (also 7) and radio_lr1110.c's documented GPIOTE choice.
 */
#include "battery.h"

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#include <hal/nrf_gpio.h>
#include <nrfx_saadc.h>

#include "battery_t1000e_conv.h"
#include "bramble_board.h"
#include "esp_log.h"

static const char* TAG = "battery_t1000e";
static bool s_initialized = false;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_done;
static StaticSemaphore_t s_done_buf;

/* Generous relative to a real oneshot conversion (tens of microseconds of
 * acquisition plus 14-bit conversion) or an offset calibration; if this
 * fires the peripheral is stuck, not slow. */
#define SAADC_WAIT_MS 25

/* Divider settle after the rail comes up. The vendor samples with no
 * explicit delay (its ADC re-init is the only gap), so the divider is fast;
 * 5 ms is margin for the unpublished divider RC at a cost of nothing at
 * beacon cadence. */
#define RAIL_SETTLE_MS 5

#define BATTERY_SAMPLE_COUNT 8

/* The nRF52840's eight SAADC inputs map to a fixed pin set (nRF52840
 * Product Specification, SAADC chapter): AIN0-AIN3 = P0.02-P0.05, AIN4-AIN7
 * = P0.28-P0.31. Spelled out and keyed off BOARD_PIN_VBAT_ADC so the board
 * header stays the single source of truth and a future board's pin choice
 * fails to compile instead of silently sampling the wrong input. */
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

static void saadc_event_handler(nrfx_saadc_evt_t const* p_event) {
    if (p_event->type == NRFX_SAADC_EVT_DONE || p_event->type == NRFX_SAADC_EVT_CALIBRATEDONE) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_done, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

void battery_init(void) {
    /* Rail gate first, and off: clear the output latch before flipping the
     * direction so the pin never glitches high, then configure the vendor's
     * exact drive (nrf_gpio_cfg_output = push-pull, standard drive S0S1,
     * matching hal_gpio_init_out in the vendor's smtc_hal_gpio.c). */
    nrf_gpio_pin_clear(BOARD_PIN_VBAT_RAIL_EN);
    nrf_gpio_cfg_output(BOARD_PIN_VBAT_RAIL_EN);

    s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);

    nrfx_err_t err = nrfx_saadc_init(NRFX_SAADC_DEFAULT_CONFIG_IRQ_PRIORITY);
    if (err != NRFX_SUCCESS) {
        ESP_LOGE(TAG, "nrfx_saadc_init failed: %d", (int)err);
        return;
    }

    nrfx_saadc_channel_t channel = NRFX_SAADC_DEFAULT_CHANNEL_SE(BATTERY_SAADC_AIN, 0);
    channel.channel_config.gain = NRF_SAADC_GAIN1_5;
    channel.channel_config.reference = NRF_SAADC_REFERENCE_INTERNAL;
    /* Longest acquisition the peripheral offers: the divider's impedance is
     * unpublished, and 40 us supports sources up to 800 kOhm (nRF52840
     * Product Specification, SAADC acquisition-time table) at a cost of 30
     * extra microseconds per sample. */
    channel.channel_config.acq_time = NRF_SAADC_ACQTIME_40US;

    err = nrfx_saadc_channel_config(&channel);
    if (err != NRFX_SUCCESS) {
        ESP_LOGE(TAG, "nrfx_saadc_channel_config failed: %d", (int)err);
        return;
    }

    err = nrfx_saadc_simple_mode_set(1u << 0, NRF_SAADC_RESOLUTION_14BIT,
                                     NRF_SAADC_OVERSAMPLE_DISABLED, saadc_event_handler);
    if (err != NRFX_SUCCESS) {
        ESP_LOGE(TAG, "nrfx_saadc_simple_mode_set failed: %d", (int)err);
        return;
    }

    /* One-shot offset calibration: an uncalibrated offset is a few LSB and
     * the 2x divider doubles it. Evented with a bounded wait like every
     * other SAADC operation here; a calibration that fails to start or
     * times out logs and continues uncalibrated, a correctness nit rather
     * than a reason to abandon boot. */
    err = nrfx_saadc_offset_calibrate(saadc_event_handler);
    if (err != NRFX_SUCCESS) {
        ESP_LOGW(TAG, "offset calibrate failed to start: %d (continuing uncalibrated)", (int)err);
    } else if (xSemaphoreTake(s_done, pdMS_TO_TICKS(SAADC_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "offset calibrate timed out (continuing uncalibrated)");
        /* Close the door deterministically: nrfx_saadc_abort() during
         * calibration abandons it without ever invoking the event handler
         * (nrfx_saadc.c's STOPPED case special-cases calibration), so no
         * stale give survives to satisfy the first real sample's wait. */
        nrfx_saadc_abort();
    }

    s_initialized = true;
    ESP_LOGI(TAG, "battery ADC ready (AIN%d via P1.06 gate, gain 1/5, 14-bit)",
             (int)(BATTERY_SAADC_AIN - NRF_SAADC_INPUT_AIN0));
}

/* File-scope, not a stack local: the conversion completes asynchronously
 * (EasyDMA writes it from the SAADC ISR after mode_trigger returns), and on
 * the timeout path a stack slot would be reclaimed memory a late write
 * could still land in. Serialized by s_lock, so one instance suffices. */
static int16_t s_raw;

/* One oneshot conversion at the pin, pre-divider. Success via the return
 * value; a failed sample is excluded from the average by the caller. */
static bool read_one_sample_mv(uint32_t* out_mv) {
    /* Drain any stale give before starting this conversion's wait: under
     * s_lock with nothing of ours in flight, a pending give can only be a
     * leftover (late calibration completion, or an aborted sample's second
     * DONE). Observed on bench, not theoretical. */
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
        /* Bounded failure, not a hang: abort the possibly-in-flight
         * conversion and drain the abort's own DONE (aborting a
         * SIMPLE_MODE sample's STOP also triggers END per nrfx_saadc.c) so
         * it cannot masquerade as the next sample's result. */
        nrfx_saadc_abort();
        ESP_LOGW(TAG, "saadc sample timed out, aborting");
        (void)xSemaphoreTake(s_done, pdMS_TO_TICKS(SAADC_WAIT_MS));
        return false;
    }
    *out_mv = battery_t1000e_raw_to_pin_mv(s_raw);
    return true;
}

uint32_t battery_read_mv(void) {
    if (!s_initialized)
        return 0;

    uint32_t samples[BATTERY_SAMPLE_COUNT];
    bool valid[BATTERY_SAMPLE_COUNT];

    /* The whole gated window runs under the lock: the rail state and the
     * nrfx_saadc global control block are both shared, and interleaving a
     * second task's window would corrupt this sample set (or release the
     * rail mid-average). The window is bounded: 5 ms settle plus at most
     * 8 * 50 ms of sample waits, and the rail-off below runs on every
     * path out, including all-samples-failed. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nrf_gpio_pin_set(BOARD_PIN_VBAT_RAIL_EN);
    vTaskDelay(pdMS_TO_TICKS(RAIL_SETTLE_MS));
    for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++)
        valid[i] = read_one_sample_mv(&samples[i]);
    nrf_gpio_pin_clear(BOARD_PIN_VBAT_RAIL_EN);
    xSemaphoreGive(s_lock);

    uint32_t pin_mv = battery_t1000e_average_mv(samples, valid, BATTERY_SAMPLE_COUNT);
    if (pin_mv == 0)
        return 0; /* all samples failed, or a genuinely dead divider */
    return battery_t1000e_pin_to_vbat_mv(pin_mv);
}

uint8_t battery_read_pct(void) { return battery_mv_to_pct(battery_read_mv()); }
