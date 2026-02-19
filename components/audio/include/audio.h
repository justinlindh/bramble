#ifndef BRAMBLE_AUDIO_H
#define BRAMBLE_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/*
 * T-Deck Plus audio hardware:
 * - ES7210 4-channel ADC (microphone) via I2S
 * - MAX98357A I2S amplifier (speaker)
 * - MCLK on GPIO48
 *
 * CONSTRAINT: When audio is active (ES7210 MCLK enabled on GPIO48),
 * do NOT simultaneously use the trackball center button (GPIO0)
 * as it shares circuitry. Enforce via software mutex.
 *
 * This is scaffolding only — full implementation deferred post-MVP.
 */

/* Initialize audio subsystem (I2S + codec setup) */
int audio_init(void);

/* Release audio resources */
void audio_deinit(void);

/* Start microphone capture to internal buffer */
int audio_record_start(void);

/* Stop microphone capture */
int audio_record_stop(void);

/* Play PCM samples through speaker */
int audio_play(const uint8_t *pcm, size_t len);

/* Check if audio hardware is available */
int audio_is_available(void);

#endif /* BRAMBLE_AUDIO_H */
