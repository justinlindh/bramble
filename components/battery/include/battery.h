#ifndef BRAMBLE_BATTERY_H
#define BRAMBLE_BATTERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Initialize battery ADC reading.
 * Heltec V3: GPIO1 with voltage divider (factor ~2).
 */
void battery_init(void);

/**
 * Read battery voltage in millivolts.
 * Returns 0 if not initialized or read fails.
 *
 * Thin wrapper over battery_get_status() (see battery_wrappers.c); kept for
 * existing call sites.
 */
uint32_t battery_read_mv(void);

/**
 * Convert millivolts to percentage (0-100).
 * Uses a LiPo discharge curve approximation.
 */
uint8_t battery_mv_to_pct(uint32_t mv);

/**
 * Convenience: read + convert in one call.
 *
 * Thin wrapper over battery_get_status() (see battery_wrappers.c); kept for
 * existing call sites.
 */
uint8_t battery_read_pct(void);

/* ── Charging-aware status (wave 2) ──────────────────────────────────────
 *
 * The plugged-in T-Deck reads a dead-flat ~4798 mV: that is the charge
 * rail, not the cell, so the old pct-only API clamped to 100% while
 * plugged in and then cliffed to the true resting voltage the moment it
 * was unplugged. battery_get_status() is the fix: it separates "how
 * charged is the cell" from "is a charger driving the rail right now" so
 * callers (beacons, RPC, displays) can stop showing a percentage that a
 * charger has made meaningless.
 */

typedef enum {
    BATTERY_CHG_UNKNOWN = 0, /* no hardware charge-detect signal on this board */
    BATTERY_CHG_NO,          /* charge-detect pin reads "not charging" */
    BATTERY_CHG_YES,         /* charge-detect pin reads "charging" */
} battery_charging_t;

typedef struct {
    uint32_t mv;                 /* averaged; 0 = unavailable */
    uint8_t pct;                 /* curve of mv */
    battery_charging_t charging; /* hardware-informed or UNKNOWN */
    bool present;                /* board has battery ADC and init succeeded */
} battery_status_t;

/**
 * Fills out with the current battery reading: averaged voltage, curve
 * percentage, and charging state. This is the primary API; battery_read_mv
 * and battery_read_pct are thin wrappers over it.
 *
 * Each target provides its own implementation: components/battery/battery.c
 * (ESP ADC), components/battery/battery_virt.c (emulator, served over
 * emu-link), nrf/shim/battery_null.c (nRF, honestly reports "no hardware"
 * until a real backend lands).
 */
void battery_get_status(battery_status_t* out);

/* Number of raw samples averaged into battery_status_t.mv on the ESP ADC
 * path (battery.c). PROVISIONAL as a plain mean: bench Task 1's trace may
 * show outliers that call for a median-of-N instead, and naming the count
 * here means that swap touches one constant, not call sites. */
#define BATTERY_AVG_SAMPLE_COUNT 8

/**
 * Pure averaging helper shared by every target that samples multiple raw
 * readings (currently just the ESP ADC path). Returns 0 for count == 0 or
 * a NULL samples pointer. No side effects, no hardware access:
 * host-testable in isolation.
 */
uint32_t battery_average_mv(const uint32_t* samples, size_t count);

/**
 * Maps a charge-status GPIO reading to a battery_charging_t. chrg_gpio < 0
 * means the board has no charge-detect pin wired, so the result is always
 * BATTERY_CHG_UNKNOWN regardless of level. Otherwise the result is YES when
 * level == chrg_active_level, NO otherwise. Pure function, no hardware
 * access: host-testable in isolation.
 */
battery_charging_t battery_charging_from_gpio(int chrg_gpio, int chrg_active_level, int level);

/**
 * Maps a status to the wire-level beacon battery_pct byte: 0xFF is the
 * protocol's documented "unknown/plugged in" sentinel
 * (docs/bramble-protocol-spec.md), emitted whenever charging is confirmed
 * OR the board has no battery hardware (present == false). Without the
 * present check, a battery-less node (nRF today) would emit a real 0,
 * which on the wire means "dead battery", not "no reading": a false
 * low-battery signal is worse than the sentinel. Otherwise the real curve
 * percentage passes through unchanged. Pure function: host-testable in
 * isolation.
 */
uint8_t battery_beacon_pct(battery_charging_t charging, uint8_t pct, bool present);

/* Percentage at/below which a battery reading is genuinely dangerous.
 * battery_display_pct_ema below must never delay or mask a drop to or
 * through this floor: a real low battery always shows immediately,
 * unsmoothed. Mirrors the UI's danger-color threshold
 * (components/ui_graphics/screens/scr_layout.c). */
#define BATTERY_DANGER_PCT 15

/*
 * Display-smoothing tunables (charging == NO or UNKNOWN path only).
 * PROVISIONAL: placeholders until Task 8's bench pass measures the actual
 * unplug settling curve (W2-T1) and finalizes them from real trace data.
 * Until then they only need to be directionally reasonable, not exact.
 */
#define BATTERY_DISPLAY_EMA_NUM 1    /* new-sample weight numerator */
#define BATTERY_DISPLAY_EMA_DEN 4    /* new-sample weight denominator */
#define BATTERY_DISPLAY_STEP_LIMIT 5 /* max displayed-pct change per call */

typedef struct {
    uint8_t displayed;
    bool has_value;
} battery_display_state_t;

/**
 * EMA display smoothing with an explicit, caller-owned state: the unplug
 * voltage cliff (the charge rail's ~4798 mV dropping to the true resting
 * cell voltage) renders as a gradual settle instead of an instant jump.
 * Floored at BATTERY_DANGER_PCT: raw_pct at or below that value is returned
 * immediately with no smoothing, so a genuinely low battery is never
 * masked by a display that is still catching up from a higher reading.
 * A NULL state returns raw_pct unsmoothed rather than crashing. Pure
 * function: host-testable in isolation.
 */
uint8_t battery_display_pct_ema(battery_display_state_t* state, uint8_t raw_pct);

/**
 * Convenience wrapper over battery_display_pct_ema using one process-wide
 * state instance. This is what UI call sites (scr_layout.c, main.c) use;
 * tests exercise battery_display_pct_ema directly with their own state so
 * cases do not leak into each other.
 */
uint8_t battery_display_pct(uint8_t raw_pct);

#endif
