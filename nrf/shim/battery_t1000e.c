/*
 * Battery backend for the SenseCAP T1000-E: gated cell-voltage reads plus
 * CHRG/VBUS charge detect. Satisfies components/battery/include/battery.h
 * (battery_get_status is the primary API; battery_read_mv/battery_read_pct
 * come from the shared battery_wrappers.c) plus the nRF arm hook in
 * shim/include/battery_nrf.h.
 *
 * VOLTAGE. The cell reaches P0.02/AIN0 through a 2x divider that hangs off
 * a sensor rail gated by P1.06 (see nrf/boards/t1000e.h for the
 * vendor-source and bench evidence), so every read is a gated window:
 * drive P1.06 high, let the divider settle, average a burst of samples
 * (battery_average_mv, valid-masked), drive it low again. That mirrors the
 * vendor's own sensor_bat_sample() (Seeed-Tracker-T1000-E-for-LoRaWAN-dev-
 * board, sensor.c: rail on, sample, times 2, rail off) with the same
 * push-pull standard-drive active-HIGH configuration its
 * hal_gpio_init_out() uses, and it keeps the rail off the rest of the
 * time, which is both the vendor's power posture and what keeps the
 * NTC/photo dividers on the same rail unpowered. An UNGATED read of this
 * pin is worse than no read: conversions succeed against the dead divider
 * and return single-digit millivolts, a confident wrong answer that
 * beacons a dying cell.
 *
 * TWO LAYERS OF PROTECTION around the one open question on this board. An
 * instrumented build once stopped dead (reset or lockup, undetermined; no
 * failure tag) the first time BOOT-context code drove P1.06, while the same
 * drive is bench-proven safe at runtime (2026-08-06 probe on this unit:
 * 2 mV off, 3920 mV driven, 2 mV released, no upset), and is what
 * Meshtastic's initVariant() does at boot fleet-wide. Because boot-vs-
 * runtime is the only known difference, this driver:
 *
 *   1. Never drives the rail in boot context. battery_init() only brings up
 *      the SAADC and parks the gate LOW; voltage reads return the mv 0
 *      "no reading" sentinel and touch no hardware until app_init calls
 *      battery_runtime_arm() after BT_BOOT_DONE. The mesh task's immediate
 *      first beacon (the boot-stage send_beacon in main/mesh_task.c)
 *      therefore emits the 0xFF unknown sentinel, and the first real gated
 *      window runs at the first post-boot poll, in the probe-proven
 *      context.
 *
 *   2. Persists a survival latch (NVS, battery_t1000e_conv.h states)
 *      around the FIRST-ever gated window: ATTEMPTING is committed before
 *      the first drive and PROVEN after the window completes, so if the
 *      drive is somehow fatal even at runtime, the next boot finds
 *      ATTEMPTING and disables the voltage path outright. Worst case is
 *      one reset per flash lifetime, self-healing, never a reset loop and
 *      never a DFU gesture. If the latch cannot be committed, the window
 *      does not run: an unrecorded fatal attempt would repeat every boot,
 *      and no reading is worth that.
 *
 * CHARGE DETECT (bench-verified live 2026-08-06: tracked plug/unplug
 * exactly). Both detect pins are plain GPIO inputs needing neither the ADC
 * nor the rail. Wiring verified against Meshtastic's
 * variants/nrf52840/tracker-t1000-e/variant.h (EXT_CHRG_DETECT (32+3),
 * EXT_PWR_DETECT (0+5)); the pin MODES are src/Power.cpp's defaults (plain
 * INPUT, no pull; charging = P1.03 LOW, external power = P0.05 HIGH),
 * confirmed by a repo-wide search finding no variant override. The vendor
 * SDK agrees (smtc_hal_config.h: CHARGER_CHRG 35, CHARGER_ADC_DET 5).
 *
 * The SAADC never busy-waits on our behalf: every operation runs in
 * event-handler (non-blocking) mode and this code does its own bounded
 * wait on a binary semaphore the event handler gives from ISR context, so
 * a stuck peripheral is a failed read, never a hung task. The
 * drain-before-wait discipline exists because a late completion from an
 * operation this code already gave up on (a slow offset calibration, an
 * aborted sample's second DONE) would otherwise satisfy the next wait
 * instantly and hand back a fabricated sample; observed on bench, not
 * hypothesized.
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
#include "battery_nrf.h"

#include <string.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#include <hal/nrf_gpio.h>
#include <nrfx_saadc.h>

#include "battery_t1000e_conv.h"
#include "boot_trace.h"
#include "bramble_board.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_keys.h"

#ifndef BOARD_PIN_VBAT_RAIL_EN
#error "this backend gates the sensor rail; the selected board maps no rail-enable pin"
#endif

/* The conversion header's divider (host-tested) and the board header's
 * divider (documentation-facing) must agree; a drift here would silently
 * scale every reading. */
_Static_assert(BATTERY_T1000E_DIVIDER == BOARD_BATTERY_DIVIDER,
               "battery_t1000e_conv.h and t1000e.h disagree on the divider");

static const char* TAG = "battery_t1000e";
static bool s_initialized = false;
static bool s_armed = false;    /* set once by battery_runtime_arm() post boot */
static bool s_disabled = false; /* latched: a previous first window never completed */
static uint8_t s_probe = BATTERY_T1000E_PROBE_UNTRIED;
static bool s_first_mv_stamped = false;

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

/* NVS key for the survival latch, in the shared bramble namespace. */
#define BATTERY_PROBE_KEY "vbat_probe"

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

static uint8_t probe_latch_load(void) {
    nvs_handle_t h;
    uint8_t v = BATTERY_T1000E_PROBE_UNTRIED;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, BATTERY_PROBE_KEY, &v);
        nvs_close(h);
    }
    return v;
}

static bool probe_latch_store(uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_u8(h, BATTERY_PROBE_KEY, v) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void battery_init(void) {
    /* Rail gate first, and off: clear the output latch before flipping the
     * direction so the pin never glitches high, then configure the vendor's
     * exact drive (nrf_gpio_cfg_output = push-pull, standard drive S0S1,
     * matching hal_gpio_init_out in the vendor's smtc_hal_gpio.c). Nothing
     * in this function, or anywhere before battery_runtime_arm(), ever
     * drives it high. */
    nrf_gpio_pin_clear(BOARD_PIN_VBAT_RAIL_EN);
    nrf_gpio_cfg_output(BOARD_PIN_VBAT_RAIL_EN);

    /* CHRG/VBUS detect: plain inputs, no pull (see the file header for the
     * Power.cpp citation backing this over a pull-up guess). */
    nrf_gpio_cfg_input(BOARD_PIN_CHRG_DETECT, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(BOARD_PIN_VBUS_DETECT, NRF_GPIO_PIN_NOPULL);

    /* Survival latch verdict from the last flash lifetime. NVS is mounted
     * before battery_init in app_init's boot order. */
    s_probe = probe_latch_load();
    if (!battery_t1000e_vbat_allowed(s_probe)) {
        s_disabled = true;
        ESP_LOGE(TAG, "previous first gated window never completed; voltage path disabled "
                      "(erase settings or reflash after diagnosis to re-arm)");
    }

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
     * than a reason to abandon boot. Touches only the converter's internal
     * offset DAC, never the rail. */
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
    ESP_LOGI(TAG, "battery ADC ready (AIN%d via P1.06 gate, gain 1/5, 14-bit), probe state %u",
             (int)(BATTERY_SAADC_AIN - NRF_SAADC_INPUT_AIN0), (unsigned)s_probe);
}

void battery_runtime_arm(void) { s_armed = true; }

uint8_t battery_probe_state(void) { return s_probe; }

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

/* One full gated window: rail up, settle, valid-masked average, rail down.
 * Returns averaged pin-level mV, 0 when no reading is available (disarmed,
 * latched off, uninitialized, latch unpersistable, or every sample
 * failed). */
static uint32_t read_gated_pin_mv(void) {
    if (!s_initialized || !s_armed || s_disabled)
        return 0;

    uint32_t samples[BATTERY_AVG_SAMPLE_COUNT];
    bool valid[BATTERY_AVG_SAMPLE_COUNT];

    /* The whole gated window runs under the lock: the rail state, the nrfx
     * global control block and the probe latch are all shared, and
     * interleaving a second task's window would corrupt this sample set
     * (or release the rail mid-average). The window is bounded: 5 ms
     * settle plus at most 8 * 50 ms of sample waits, and the rail-off
     * below runs on every path out, including all-samples-failed. */
    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool first_window = (s_probe == BATTERY_T1000E_PROBE_UNTRIED);
    if (first_window && !probe_latch_store(BATTERY_T1000E_PROBE_ATTEMPTING)) {
        /* Cannot record the attempt: refuse the window. An unrecorded
         * fatal attempt would repeat every boot, which is the reset loop
         * the latch exists to prevent; no reading is worth that. */
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "cannot persist rail-probe latch; skipping voltage read");
        return 0;
    }

    nrf_gpio_pin_set(BOARD_PIN_VBAT_RAIL_EN);
    vTaskDelay(pdMS_TO_TICKS(RAIL_SETTLE_MS));
    for (int i = 0; i < BATTERY_AVG_SAMPLE_COUNT; i++)
        valid[i] = read_one_sample_mv(&samples[i]);
    nrf_gpio_pin_clear(BOARD_PIN_VBAT_RAIL_EN);

    if (first_window) {
        /* Survival, not sample quality, is the latch's claim: the window
         * completed and the board is alive, so the drive is proven on this
         * unit regardless of how the samples came out. If the PROVEN store
         * fails, the next boot repeats the ATTEMPTING/PROVEN pair; the
         * in-memory state still advances so this boot never re-runs it. */
        if (!probe_latch_store(BATTERY_T1000E_PROBE_PROVEN))
            ESP_LOGW(TAG, "rail-probe PROVEN latch store failed; will retry next boot");
        s_probe = BATTERY_T1000E_PROBE_PROVEN;
    }
    xSemaphoreGive(s_lock);

    return battery_average_mv(samples, valid, BATTERY_AVG_SAMPLE_COUNT);
}

void battery_get_status(battery_status_t* out) {
    memset(out, 0, sizeof(*out));

    if (!s_initialized) {
        out->charging = BATTERY_CHG_UNKNOWN;
        return;
    }

    /* Charge detect first: plain GPIO reads, valid from init on, needing
     * neither the rail nor the ADC. VBUS folds into the single "charging"
     * signal because battery_status_t has no separate "externally powered
     * but not charging" state; Meshtastic's isVbusIn() (src/Power.cpp) is
     * this same OR, for the same reason: EXT_PWR_DETECT alone misses a
     * charge-complete pin that de-asserts CHRG while a charger is still
     * connected. */
    int chrg_level = nrf_gpio_pin_read(BOARD_PIN_CHRG_DETECT);
    battery_charging_t chrg =
        battery_charging_from_gpio(BOARD_PIN_CHRG_DETECT, 0 /* active LOW */, chrg_level);
    bool vbus_present = nrf_gpio_pin_read(BOARD_PIN_VBUS_DETECT) != 0;
    out->charging = (chrg == BATTERY_CHG_YES || vbus_present) ? BATTERY_CHG_YES : BATTERY_CHG_NO;

    /* present per battery.h: the hardware exists and init succeeded. While
     * the voltage path is disarmed (boot window) or latched off, mv stays
     * 0, the documented "no reading" sentinel, so
     * battery_reading_available() is false and beacons emit 0xFF instead
     * of a fabricated percentage. */
    out->present = true;

    uint32_t pin_mv = read_gated_pin_mv();
    if (pin_mv != 0) {
        out->mv = battery_t1000e_pin_to_vbat_mv(pin_mv);
        out->pct = battery_mv_to_pct(out->mv);

        /* One trace stamp per boot with the first successful reading, so a
         * decoded page shows what the cell measured without a console. */
        if (!s_first_mv_stamped) {
            s_first_mv_stamped = true;
            boot_trace_mark(BT_BATTERY_MV, out->mv);
        }
    }

    /* No-op here (this board's charge/VBUS pins mean the verdict above is
     * already YES or NO, never UNKNOWN), but run through the shared
     * voltage-inference step anyway so every battery_get_status
     * implementation funnels through the same decision. */
    out->charging = battery_infer_charging(out->charging, out->mv);
}
