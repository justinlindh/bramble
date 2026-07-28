/*
 * The glue Apache NimBLE needs to run host + controller on this target with
 * no Mynewt OS and no SoftDevice. Three pieces, all of which upstream expects
 * the integrator to supply:
 *
 *  1. ble_npl_hw_set_isr: the link layer installs its own RADIO/RNG/RTC0
 *     handlers at runtime, so the vector table entries trampoline through
 *     function pointers. (The RIOT port does the same; the FreeRTOS port
 *     ships no implementation at all.)
 *  2. nrf52_clock_hfxo_request/release: the PHY ramps the 32MHz crystal
 *     around radio events. Refcounted because TX and RX paths both ask.
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

/* The radio needs the crystal's accuracy: a caller that proceeds on the
 * internal RC oscillator transmits slightly off-frequency and misses tight
 * receive windows (advertising survives that, catching a CONNECT_IND 150us
 * after an advertisement does not, which reads as "visible in every scan,
 * never connectable").
 *
 * So the crystal is started once, at BLE bring-up, and left running: this
 * function is called from the link layer's ISR on every radio event, and
 * spinning there for the ~360us startup starves every task in the system
 * (measured: the mesh stopped draining its receive queue and the heartbeat
 * task stopped entirely within seconds). Keeping HFXO on costs idle current,
 * which is P3's problem to solve with a proper power model, not something to
 * buy here with a busy-wait in an interrupt.
 *
 * There is deliberately no refcount. Nothing stops the crystal, so a count
 * would have no reader, and maintaining one meant masking interrupts inside
 * a function the link layer calls from its ISR on every radio event. Both
 * register writes below are idempotent, so two contexts repeating them race
 * harmlessly.
 *
 * Honest caveat for whoever owns the power model (P3): leaving HFXO on costs
 * roughly 460uA continuously, and the "we would have to spin in an ISR"
 * argument does not actually justify keeping it that way. NimBLE's rfclk
 * management requests the clock BLE_LL_RFMGMT_ENABLE_TIME (1500us) ahead of
 * a radio event, which is over four times the ~360us startup, so a real
 * release path would have time to start it without any spin. That work needs
 * a power model and bench measurement, which is why it is not being done
 * here, but the reason to revisit is stronger than this comment first
 * implied.
 */
void nrf52_clock_hfxo_request(void) {
    if (!nrf_clock_hf_is_running(NRF_CLOCK, NRF_CLOCK_HFCLK_HIGH_ACCURACY)) {
        nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED);
        nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTART);
    }
}

void nrf52_clock_hfxo_release(void) {
    /* Deliberately does NOT stop the crystal; see the request path. */
}

/* ------------------------------------------------------------------ */
/*  Low-frequency clock                                                */
/* ------------------------------------------------------------------ */

bool nimble_glue_start_lfclk(void) {
    /* The high-frequency crystal goes first, and above the low-frequency
     * early return below rather than after it. Waiting for HFXO is only safe
     * in task context, and this is the one place that runs there; if the
     * low-frequency clock happens to be up already (a reset that leaves CLOCK
     * configured), returning early would leave the crystal to
     * nrf52_clock_hfxo_request, which the radio ISR calls and which
     * deliberately does not wait. The first radio events would then run on
     * the RC oscillator, which is exactly the "visible in every scan, never
     * connectable" failure described above.
     */
    if (!nrf_clock_hf_is_running(NRF_CLOCK, NRF_CLOCK_HFCLK_HIGH_ACCURACY)) {
        nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED);
        nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTART);
        while (!nrf_clock_hf_is_running(NRF_CLOCK, NRF_CLOCK_HFCLK_HIGH_ACCURACY)) {
        }
    }

    if (nrf_clock_lf_is_running(NRF_CLOCK)) {
        return nrf_clock_lf_actv_src_get(NRF_CLOCK) == NRF_CLOCK_LFCLK_XTAL;
    }

    nrf_clock_lf_src_set(NRF_CLOCK, NRF_CLOCK_LFCLK_XTAL);
    nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_LFCLKSTARTED);
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_LFCLKSTART);

    /* The crystal takes under a second when fitted; if the board has none,
     * this never fires and the RC oscillator is the honest fallback. */
    for (int i = 0; i < 1000000; i++) {
        if (nrf_clock_event_check(NRF_CLOCK, NRF_CLOCK_EVENT_LFCLKSTARTED)) {
            ESP_LOGI(TAG, "LFCLK running on the 32.768kHz crystal");
            return true;
        }
    }

    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_LFCLKSTOP);
    nrf_clock_lf_src_set(NRF_CLOCK, NRF_CLOCK_LFCLK_RC);
    nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_LFCLKSTARTED);
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_LFCLKSTART);
    while (!nrf_clock_event_check(NRF_CLOCK, NRF_CLOCK_EVENT_LFCLKSTARTED)) {
    }
    /* The RC source needs periodic calibration to hold its accuracy. */
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_CAL);
    ESP_LOGW(TAG, "no 32.768kHz crystal, LFCLK running on the RC oscillator");
    return false;
}
