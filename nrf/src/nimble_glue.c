/*
 * The glue Apache NimBLE needs to run host + controller on this target with
 * no Mynewt OS and no SoftDevice. Three pieces, all of which upstream expects
 * the integrator to supply:
 *
 *  1. ble_npl_hw_set_isr: the link layer installs its own RADIO/RNG/RTC0
 *     handlers at runtime, so the vector table entries trampoline through
 *     function pointers. (The RIOT port does the same; the FreeRTOS port
 *     ships no implementation at all.)
 *  2. nrf52_clock_hfxo_request/release: declared for NimBLE's nrf5x PHY but
 *     dead code on this port, since that PHY only calls them under
 *     MYNEWT/RIOT_VERSION/PEBBLEOS, none of which this FreeRTOS build
 *     defines; see the comment above their definitions for what actually
 *     cycles the crystal here.
 *  3. LFCLK startup: RTC0 (the controller's 32768Hz time base) does not run
 *     without it, and nothing else on this target starts it. Crystal first,
 *     RC as a fallback, and the caller learns which so BLE_LL_SCA can be
 *     honest about the accuracy.
 *
 * Peripheral ownership note: the controller claims RADIO, TIMER0, RTC0, CCM,
 * AAR, ECB and RNG plus several PPI channels. Nothing else in this firmware
 * may use those (see nrf/config/nrfx_glue.h).
 */
#include "nimble_glue.h"

#include "boot_trace.h"

#include <nrfx.h>

#include <hal/nrf_clock.h>

#include "esp_log.h"

static const char* TAG = "nimble_glue";

/* ------------------------------------------------------------------ */
/*  ISR trampolines                                                    */
/* ------------------------------------------------------------------ */

static void (*s_radio_isr)(void);
static void (*s_rng_isr)(void);
static void (*s_rtc0_isr)(void);

void RADIO_IRQHandler(void) {
    if (s_radio_isr) {
        s_radio_isr();
    }
}

void RNG_IRQHandler(void) {
    if (s_rng_isr) {
        s_rng_isr();
    }
}

void RTC0_IRQHandler(void) {
    if (s_rtc0_isr) {
        s_rtc0_isr();
    }
}

/* The FreeRTOS NPL's inline ble_npl_hw_set_isr forwards here. */
void npl_freertos_hw_set_isr(int irqn, void (*addr)(void)) {
    switch (irqn) {
    case RADIO_IRQn:
        s_radio_isr = addr;
        break;
    case RNG_IRQn:
        s_rng_isr = addr;
        break;
    case RTC0_IRQn:
        s_rtc0_isr = addr;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  High-frequency crystal                                             */
/* ------------------------------------------------------------------ */

/* Vestigial in this build. NimBLE's nrf5x PHY (nimble/drivers/nrf5x/src/
 * ble_phy.c, ble_phy_rfclk_enable/disable) only calls these two functions
 * under `#if MYNEWT || defined(RIOT_VERSION) || defined(PEBBLEOS)`; none of
 * those are defined for this FreeRTOS/ESP-IDF-shim port, so the compiler
 * takes the #else arm instead: a direct `nrf_clock_task_trigger(NRF_CLOCK,
 * NRF_CLOCK_TASK_HFCLKSTART)` / `..._HFCLKSTOP`. Confirmed by disassembling
 * the shipped ELF: ble_phy_rfclk_enable/disable are four instructions each,
 * a single register store to the CLOCK peripheral base, no `bl` into this
 * file at all.
 *
 * The real owner of HFXO cycling on this port is therefore unmodified
 * upstream NimBLE: nimble/controller/src/ble_ll_rfmgmt.c already requests
 * the crystal MYNEWT_VAL_BLE_LL_RFMGMT_ENABLE_TIME (1500us, see
 * nrf/config/nimble/syscfg/syscfg.h) ahead of each radio event and stops it
 * afterward, through those same raw ble_phy_rfclk_* writes. HFXO already
 * cycles off between BLE events without anything in this file, and that
 * was true before the HFXO-release task that added this comment, not
 * something the task delivered.
 *
 * These two functions are kept as an integration point for a MYNEWT/RIOT/
 * PEBBLEOS build variant, or for a second HFXO consumer other than the
 * radio if one ever appears on this port (SAADC does not need HFXO for its
 * own conversions; there is no USBD on this target, which would). If
 * either shows up, ble_phy_rfclk_enable/disable need a one-line patch in
 * nrf/patches/ to route through these functions instead of the raw
 * register writes, the same mechanism already used for
 * nimble-isr-safe-critical.patch and nimble-dup-pdu-during-enc-start.patch.
 */
void nrf52_clock_hfxo_request(void) {
    if (!nrf_clock_hf_is_running(NRF_CLOCK, NRF_CLOCK_HFCLK_HIGH_ACCURACY)) {
        nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED);
        nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTART);
    }
}

void nrf52_clock_hfxo_release(void) {
    /* Deliberately does NOT stop the crystal. With no live caller in this
     * build there is no real usage pattern to refcount against; inventing
     * one here would be guessing at semantics a future real caller should
     * define instead. */
}

/* ------------------------------------------------------------------ */
/*  Low-frequency clock                                                */
/* ------------------------------------------------------------------ */

/* nRF52 anomaly 192: LFRC calibration produces a frequency error above the
 * datasheet's specified 500ppm without this undocumented-register write.
 * No symbolic name for it exists in the public nRF52 SVD/HAL; nrfx's own
 * driver pokes the same raw address (drivers/src/nrfx_clock.c:514,722),
 * which is not linked into this target (this file calls the HAL directly
 * instead), so the workaround has to be repeated here by hand. */
#define NRF52_ANOMALY_192_REG ((volatile uint32_t*)0x40000C34)

bool nimble_glue_start_lfclk(void) {
    /* This still blocks on HFXO directly, not through
     * nrf52_clock_hfxo_request (dead code on this port; see above),
     * because task context is the one safe place in the boot path to spin,
     * and LFCLK bring-up needs HFXO itself for the RC-calibration branch
     * below. It is a local, single-owner start/stop scoped to this
     * function's own window: nothing else here depends on the crystal, so
     * there is nothing to refcount, only start-before-use and
     * stop-after-use on every exit path.
     *
     * Stopping HFXO before returning (this function used to leave it
     * running) shrinks the boot-time window it is on for no reason: with
     * HFXO cycling already owned by NimBLE's rfmgmt (see above), the gap
     * this closes is only between LFCLK bring-up finishing and rfmgmt's
     * own first enable/release cycle, which can be well after boot and
     * long before any radio activity actually needs the crystal. Once BLE
     * activity starts, rfmgmt owns cycling exactly as it always has.
     */
    nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED);
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTART);
    while (!nrf_clock_hf_is_running(NRF_CLOCK, NRF_CLOCK_HFCLK_HIGH_ACCURACY)) {
    }

    boot_trace_mark(BT_HFXO_OK, 0);

    if (nrf_clock_lf_is_running(NRF_CLOCK)) {
        bool xtal = nrf_clock_lf_actv_src_get(NRF_CLOCK) == NRF_CLOCK_LFCLK_XTAL;
        nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTOP);
        return xtal;
    }

    nrf_clock_lf_src_set(NRF_CLOCK, NRF_CLOCK_LFCLK_XTAL);
    nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_LFCLKSTARTED);
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_LFCLKSTART);

    /* The crystal takes under a second when fitted; if the board has none,
     * this never fires and the RC oscillator is the honest fallback. */
    for (int i = 0; i < 1000000; i++) {
        if (nrf_clock_event_check(NRF_CLOCK, NRF_CLOCK_EVENT_LFCLKSTARTED)) {
            ESP_LOGI(TAG, "LFCLK running on the 32.768kHz crystal");
            nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTOP);
            return true;
        }
    }

    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_LFCLKSTOP);
    nrf_clock_lf_src_set(NRF_CLOCK, NRF_CLOCK_LFCLK_RC);
    nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_LFCLKSTARTED);
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_LFCLKSTART);
    while (!nrf_clock_event_check(NRF_CLOCK, NRF_CLOCK_EVENT_LFCLKSTARTED)) {
    }

    /* The RC source needs calibration against HFCLK to hold its accuracy,
     * which is the one real dependency this function has on HFXO: the
     * calibration task requires the high-accuracy HFCLK running for its
     * duration. The wait below is bounded rather than an unconditional
     * spin: if calibration does not complete, boot proceeds with an
     * uncalibrated RC oscillator rather than hanging forever.
     * MYNEWT_VAL_BLE_LL_SCA (nrf/config/nimble/syscfg/syscfg.h) is a fixed
     * 250ppm figure chosen for a calibrated RC fallback; an uncalibrated
     * LFRC can drift past that, and the link layer has no runtime signal
     * to learn it did not get calibrated, so the honest consequence of
     * hitting this fallback is a higher risk of missed connection events
     * until the next periodic recalibration. The 1000000-iteration bound
     * reuses the same unscaled loop budget as the crystal-detection wait
     * above rather than a datasheet CAL-duration figure; no such figure was
     * looked up for this bound; it is a generous margin, not a measurement.
     */
    nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_DONE);
    *NRF52_ANOMALY_192_REG = 0x00000002;
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_CAL);
    bool calibrated = false;
    for (int i = 0; i < 1000000; i++) {
        if (nrf_clock_event_check(NRF_CLOCK, NRF_CLOCK_EVENT_DONE)) {
            calibrated = true;
            break;
        }
    }
    nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_DONE);
    *NRF52_ANOMALY_192_REG = 0x00000000;
    if (!calibrated) {
        ESP_LOGW(TAG, "LFRC calibration did not complete, proceeding uncalibrated");
    }

    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTOP);
    ESP_LOGW(TAG, "no 32.768kHz crystal, LFCLK running on the RC oscillator");
    return false;
}

/* ------------------------------------------------------------------ */
/*  Audit: other HFCLK/HFXO touchers in nrf/                           */
/* ------------------------------------------------------------------ */

/* This file, including this function's own local start/stop above, is the
 * only place in nrf/ that touches the CLOCK peripheral's HFCLK tasks or
 * events; grepping the tree for HFCLK/hfxo/HFXO outside this file and its
 * header turns up nothing else. Peripherals were checked against HFXO
 * cycling on and off between BLE events, which NimBLE's rfmgmt already did
 * before this file changed (see above):
 *
 *  - Console UARTE (nrf/shim/console_uart.c): non-RADIO peripherals derive
 *    their PCLK from whichever HFCLK source is currently active: HFINT
 *    (the internal RC oscillator) whenever HFXO is stopped, which per
 *    rfmgmt is most of the time BLE is idle. 115200 8n1 needs combined
 *    transmit/receive clock accuracy within roughly 2 percent. The nRF52840
 *    Product Specification lists HFINT's fTOL_HFINT as +/-1.5 percent
 *    typical but +/-8 percent at the guaranteed max corner, four times over
 *    that budget: the typical corner stays in budget on HFINT with no code
 *    change, the guaranteed-max corner does not, and nothing here has
 *    measured which corner the devkit's part actually sits at. Bench
 *    verification of console integrity under real HFXO cycling is Task 8's
 *    job, not this comment's.
 *  - SAADC (battery voltage sampling, nrf/shim/battery_saadc.c, the
 *    T1000-E's real ADC + charge-detect backend): does not need HFXO for
 *    its own conversion timing, unaffected by HFXO cycling the same way
 *    UARTE and SPIM2 below are.
 *  - SPIM2 (the LR1110 radio's SPI bus, nrf/src/lr11xx_hal_nrf.c, 8MHz):
 *    same as UARTE, HFCLK-domain-sourced from whichever source is active
 *    rather than requiring HFXO specifically, so unaffected by HFXO
 *    cycling on/off the same way UARTE is. There is no USBD instance on
 *    this target (USBD does require HFXO); that only matters the day this
 *    target grows a USB peripheral.
 */
