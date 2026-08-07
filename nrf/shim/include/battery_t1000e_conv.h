/*
 * Pure conversion math for the T1000-E battery path: SAADC raw code to
 * millivolts at the pin, pin millivolts to cell millivolts, and the
 * rail-probe survival-latch states. Averaging is NOT here: the driver uses
 * the fleet-shared battery_average_mv (components/battery/battery_helpers.c,
 * host-tested in test/test_battery.c). No peripheral access and no OS
 * dependency so the host suite (test/test_battery_t1000e.c) exercises
 * exactly the arithmetic the device runs; nrf/shim/battery_t1000e.c is the
 * only other includer.
 *
 * Scaling: the channel samples with gain 1/5 against the 0.6V internal
 * reference, so full scale is 0.6 / (1/5) = 3.0V (nRF52840 Product
 * Specification, SAADC chapter, "Digital output": RESULT = V(P) * GAIN /
 * REFERENCE * 2^RESOLUTION). A 14-bit oneshot code in [0, 16383] maps
 * linearly onto [0, 3000) mV at the pin. The board divides the cell by 2
 * before the pin (vendor sensor.c multiplies its sample by 2; Meshtastic
 * ADC_MULTIPLIER 2.0F agrees), so cell mV is pin mV times 2.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BATTERY_T1000E_FULL_SCALE_MV 3000u
#define BATTERY_T1000E_RESOLUTION_COUNTS 16384u /* 2^14, NRF_SAADC_RESOLUTION_14BIT */
#define BATTERY_T1000E_DIVIDER 2u

/* One raw SAADC code to millivolts at the pin. Negative codes are rail
 * noise near 0V (the converter is bipolar in single-ended mode) and clamp
 * to 0; codes at or above full scale clamp to the top so the function is
 * total even for inputs the 14-bit peripheral cannot produce. */
static inline uint32_t battery_t1000e_raw_to_pin_mv(int16_t raw) {
    if (raw < 0)
        raw = 0;
    uint32_t code = (uint32_t)raw;
    if (code >= BATTERY_T1000E_RESOLUTION_COUNTS)
        code = BATTERY_T1000E_RESOLUTION_COUNTS - 1u;
    return (code * BATTERY_T1000E_FULL_SCALE_MV) / BATTERY_T1000E_RESOLUTION_COUNTS;
}

/* Pin millivolts to cell millivolts through the board's 2x divider. */
static inline uint32_t battery_t1000e_pin_to_vbat_mv(uint32_t pin_mv) {
    return pin_mv * BATTERY_T1000E_DIVIDER;
}

/* Survival-latch states persisted in NVS (u8). The latch exists because one
 * boot-context drive of the P1.06 rail stopped the board dead while the
 * same drive is bench-proven at runtime: before the first-ever gated window
 * the driver persists ATTEMPTING, and persists PROVEN once the window
 * completes (survival is the claim, not sample quality). A boot that finds
 * ATTEMPTING knows the previous attempt died mid-window and disables the
 * voltage path, so even a drive that is fatal in every context costs one
 * reset ever, never a loop. */
#define BATTERY_T1000E_PROBE_UNTRIED 0u
#define BATTERY_T1000E_PROBE_ATTEMPTING 1u
#define BATTERY_T1000E_PROBE_PROVEN 2u

/* Whether the stored latch state permits driving the rail this boot. Only a
 * recorded died-mid-window verdict forbids it; an unknown or corrupt value
 * is treated as untried rather than bricking the feature on garbage. */
static inline bool battery_t1000e_vbat_allowed(uint8_t stored) {
    return stored != BATTERY_T1000E_PROBE_ATTEMPTING;
}
