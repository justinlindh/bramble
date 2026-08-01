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

/* ── Charging-aware status ────────────────────────────────────────────────
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
 * emu-link), nrf/shim/battery_saadc.c (T1000-E, real SAADC + charge
 * detect), nrf/shim/battery_null.c (WM1110 dev kit, no battery hardware
 * wired: honestly reports present=false).
 */
void battery_get_status(battery_status_t* out);

/* Number of raw samples averaged into battery_status_t.mv on the ESP and
 * SAADC ADC paths. A plain mean, not a median: bench Task 1's 30-minute
 * trace showed a steady-state reading within +/-4 mV with no outlier
 * spikes, so there is nothing for a median to reject. Named so a future
 * board whose ADC noise profile actually needs one touches this constant
 * (and battery_average_mv's implementation), not call sites. */
#define BATTERY_AVG_SAMPLE_COUNT 8

/**
 * Pure averaging helper shared by every target that samples multiple raw
 * readings (the ESP and SAADC ADC paths). Averages only the entries whose
 * matching valid[i] is true, so one failed conversion cannot drag a
 * healthy reading toward a false low-battery value the way including it
 * as a fabricated 0 mV sample would: with BATTERY_AVG_SAMPLE_COUNT == 8, a
 * single such zero pulls a real 4010 mV average down to 3509 mV, a false
 * drop from 83% to 30% that lands on the wire via the beacon and bypasses
 * battery_display_pct_ema's danger floor (which only ever sees the
 * already-corrupted average). valid may be NULL, meaning every sample is
 * treated as valid (the historical unconditional-mean behavior). Returns 0 when
 * count == 0, samples is NULL, or no sample is valid: this is exactly what
 * battery_status_t.mv's "0 = unavailable" means, a real signal that no
 * reading could be trusted, not a coincidental low value. No side effects,
 * no hardware access: host-testable in isolation.
 */
uint32_t battery_average_mv(const uint32_t* samples, const bool* valid, size_t count);

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
 * Display-smoothing tunables (charging == NO or UNKNOWN path only), chosen
 * and final from bench Task 1's measured unplug trace: the T-Deck's
 * charge-rail clamp (~4798 mV, 100%) drops to the true resting-cell
 * reading (~4010 mV, 83%) in a single ADC sample, an artifact step of
 * roughly 17 percentage points at the moment of unplug, not a gradual
 * discharge. EMA_NUM/DEN of 1/4 with a 5-point STEP_LIMIT walks that step
 * down in about 10 calls (~20s at the T-Deck LVGL status bar's 2s render
 * cadence): fast enough that a user watching the screen sees a deliberate
 * settle rather than a stuck reading, slow enough to read as smoothing
 * rather than a second jump. The EMA's job is masking this one-time rail
 * artifact, not tracking the subsequent steady-state wobble (bench-measured
 * at +/-4 mV, itself under a percentage point). Task 8 verifies this
 * against real hardware; it is not expected to change these constants.
 */
#define BATTERY_DISPLAY_EMA_NUM 1    /* new-sample weight numerator */
#define BATTERY_DISPLAY_EMA_DEN 4    /* new-sample weight denominator */
#define BATTERY_DISPLAY_STEP_LIMIT 5 /* max displayed-pct change per call */

/* Above this gap between calls, battery_display_pct_ema snaps to raw_pct
 * instead of smoothing. The EMA constants above are tuned to walk down the
 * T-Deck's unplug artifact over roughly 10 calls at a 2s LVGL cadence
 * (~20s); at a slower cadence the same call-count budget stretches out
 * proportionally (e.g. ~10 minutes at a 60s e-paper refresh), well past the
 * few-second window the smoothing exists to bridge. 10s covers every
 * display's normal cadence (LVGL ~2s, OLED ~1s) while catching the coarser
 * ones (e-paper, or any caller that pauses updates) where there is no
 * longer a fast rail-step artifact left to mask, just a real reading that
 * should show immediately. */
#define BATTERY_DISPLAY_SNAP_INTERVAL_MS 10000u

typedef struct {
    uint8_t displayed;
    bool has_value;
    uint32_t last_call_ms;
} battery_display_state_t;

/**
 * EMA display smoothing with an explicit, caller-owned state: the unplug
 * voltage cliff (the charge rail's ~4798 mV dropping to the true resting
 * cell voltage) renders as a gradual settle instead of an instant jump.
 * now_ms is the caller's monotonic clock reading (any wraparound-safe
 * millisecond tick; callers use esp_timer_get_time()/1000): a gap since
 * the previous call larger than BATTERY_DISPLAY_SNAP_INTERVAL_MS snaps to
 * raw_pct instead of smoothing, since the artifact the EMA masks is a
 * few-seconds-scale rail step, not something a slow-cadence caller (e.g.
 * an e-paper display) needs walked in gradually. Floored at
 * BATTERY_DANGER_PCT: raw_pct at or below that value is returned
 * immediately with no smoothing, so a genuinely low battery is never
 * masked by a display that is still catching up from a higher reading.
 * A NULL state returns raw_pct unsmoothed rather than crashing. Pure
 * function: host-testable in isolation.
 */
uint8_t battery_display_pct_ema(battery_display_state_t* state, uint8_t raw_pct, uint32_t now_ms);

/**
 * Convenience wrapper over battery_display_pct_ema using one process-wide
 * state instance and esp_timer_get_time() as the clock (see
 * battery_wrappers.c). This is what UI call sites (scr_layout.c, main.c)
 * use; tests exercise battery_display_pct_ema directly with their own
 * state and explicit now_ms so cases do not leak into each other.
 */
uint8_t battery_display_pct(uint8_t raw_pct);

#endif
