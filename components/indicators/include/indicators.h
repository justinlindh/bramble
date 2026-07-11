#ifndef BRAMBLE_INDICATORS_H
#define BRAMBLE_INDICATORS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * indicators: the pager's alert outputs (notification LED, buzzer, vibration
 * motor) behind one small interface. Two implementations present it upward:
 *
 *   - indicators.c   (on-device): a trivial GPIO/LEDC wrapper. Pager pins are
 *     LED = GPIO48, buzzer = GPIO15 (LEDC tone), vibra = GPIO16.
 *   - indicator_virt.c (host/emulator): each change emits an emu-link `ind`
 *     message carrying the full (led, buzzer_hz, vibra) state so the frontend
 *     always has a complete snapshot.
 */

/* Configure the outputs (device: GPIO/LEDC setup; host: no-op). */
void indicator_init(void);

/* Notification LED on/off. */
void indicator_set_led(bool on);

/* Buzzer tone at hz_or_0 Hz; 0 silences it. */
void indicator_buzzer(uint32_t hz_or_0);

/* Vibration motor on/off. */
void indicator_vibra(bool on);

#ifdef __cplusplus
}
#endif

#endif /* BRAMBLE_INDICATORS_H */
