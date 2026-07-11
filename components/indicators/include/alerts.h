#ifndef BRAMBLE_ALERTS_H
#define BRAMBLE_ALERTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * alerts: the pager's message-alert pattern, driven over the indicators
 * interface (indicators.h). Pure timing logic with no OS or driver
 * dependencies: callers feed it a millisecond clock via alerts_tick() from
 * their main loop, so the same code runs on device, on the emulator node,
 * and in the host unit tests.
 *
 * Pattern on alerts_message_received(): two buzzer beeps (200 ms on, 150 ms
 * gap, 200 ms on) at ALERT_BUZZER_HZ with a single 400 ms vibration pulse
 * starting at the same moment. The notification LED is level-driven by
 * alerts_set_unread(): lit while unread messages exist, dark once read
 * (classic pager behavior).
 */

#define ALERT_BUZZER_HZ 3200u
#define ALERT_BEEP_ON_MS 200u
#define ALERT_BEEP_GAP_MS 150u
#define ALERT_VIBRA_MS 400u

/* Reset internal state (idle, LED dark). Does not touch the outputs. */
void alerts_init(void);

/* A new incoming message arrived: start (or restart) the alert pattern. */
void alerts_message_received(uint32_t now_ms);

/* Level-driven notification LED: lit while any message is unread. */
void alerts_set_unread(bool any_unread);

/* Advance the pattern; call from the main loop (50 ms cadence is plenty). */
void alerts_tick(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* BRAMBLE_ALERTS_H */
