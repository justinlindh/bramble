/**
 * Sleep manager for display power saving
 *
 * Automatically turns off display after inactivity timeout.
 * Mesh continues to operate normally while asleep.
 */

#ifndef BRAMBLE_SLEEP_MANAGER_H
#define BRAMBLE_SLEEP_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize sleep manager.
 * Loads persisted preferences and starts activity timer.
 * Returns 0 on success, -1 on failure.
 */
int sleep_manager_init(void);

/**
 * Deinitialize sleep manager.
 * Stops timer and releases resources.
 */
void sleep_manager_deinit(void);

/**
 * Signal user activity (keypress, touch, etc.).
 * Resets sleep timer and wakes display if asleep.
 */
void sleep_manager_activity(void);

/**
 * Drive pending sleep transitions from the UI task.
 * The inactivity timer only flags that the timeout elapsed; this performs the
 * actual (blocking, SPI-bound) display power-down, so it must be called from
 * the UI/LVGL task and never from the timer callback. Call periodically.
 */
void sleep_manager_process(void);

/**
 * Check if display is currently in sleep mode.
 * Returns true if asleep, false if awake.
 */
bool sleep_manager_is_asleep(void);

/**
 * Enable or disable automatic sleep mode.
 * Persists to NVS.
 */
void sleep_manager_set_enabled(bool enabled);

/**
 * Check if automatic sleep mode is enabled.
 */
bool sleep_manager_get_enabled(void);

/**
 * Set inactivity timeout in seconds (1-3600).
 * Persists to NVS.
 */
void sleep_manager_set_timeout(uint16_t seconds);

/**
 * Get current inactivity timeout in seconds.
 */
uint16_t sleep_manager_get_timeout(void);

#endif /* BRAMBLE_SLEEP_MANAGER_H */
