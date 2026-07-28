/*
 * Force-included into the NimBLE build (see nrf/CMakeLists.txt).
 *
 * Two upstream files call integrator-provided functions without a header
 * that declares them (they come from Mynewt MCU packages this port does not
 * use). Declaring them here keeps the upstream tree unpatched and avoids
 * shadowing its own headers with local copies.
 */
#pragma once

/* npl_os_freertos.c uses SCB->ICSR to detect ISR context but includes no
 * CMSIS header of its own; nrfx.h brings in the core definitions. */
#include <nrfx.h>

/* The NPL header defines ble_npl_hw_set_isr as a static inline that
 * forwards here, so only the forwardee needs declaring. */
void npl_freertos_hw_set_isr(int irqn, void (*addr)(void));
void nrf52_clock_hfxo_request(void);
void nrf52_clock_hfxo_release(void);
