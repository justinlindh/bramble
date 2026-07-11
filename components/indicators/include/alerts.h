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
 * starting at the same moment. The notification LED then BLINKS (a short
 * pulse every couple of seconds, classic pager, battery-friendly) until the
 * user confirms on the device: any button press calls alerts_confirm(),
 * which stops the blink.
 */

#define ALERT_BUZZER_HZ 3200u
#define ALERT_BEEP_ON_MS 200u
#define ALERT_BEEP_GAP_MS 150u
#define ALERT_VIBRA_MS 400u
#define ALERT_LED_ON_MS 200u     /* blink pulse width */
#define ALERT_LED_PERIOD_MS 2000u /* blink period while unconfirmed */

/* Reset internal state (idle, LED dark). Does not touch the outputs. */
void alerts_init(void);

/* A new incoming message arrived: start (or restart) the alert pattern and
 * arm the unconfirmed-blink until alerts_confirm(). */
void alerts_message_received(uint32_t now_ms);

/* The user acknowledged on the device (any button press): stop the blink. */
void alerts_confirm(void);

/* True while a message alert is awaiting acknowledgement (LED blinking).
 * Callers use this to make the FIRST button press after an alert a pure
 * acknowledge (consumed, no navigation), like a physical pager. */
bool alerts_unconfirmed(void);

/* Advance the pattern + blink; call from the main loop (50 ms cadence). */
void alerts_tick(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* BRAMBLE_ALERTS_H */
