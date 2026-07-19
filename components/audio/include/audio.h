#ifndef BRAMBLE_AUDIO_H
#define BRAMBLE_AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Alert tone types */
typedef enum {
    AUDIO_TONE_MESSAGE_RX = 0, /* New message received: short double beep */
    AUDIO_TONE_MESSAGE_TX,     /* Message sent: single rising tone */
    AUDIO_TONE_PEER_JOIN,      /* New peer discovered: pleasant chime */
    AUDIO_TONE_PEER_LEAVE,     /* Peer lost: descending tone */
    AUDIO_TONE_ERROR,          /* Error/warning: harsh buzz */
    AUDIO_TONE_BOOT,           /* Boot complete: ascending arpeggio */
    AUDIO_TONE_GPS_FIX,        /* GPS fix acquired: two soft beeps */
} audio_tone_t;

/* Initialize I2S speaker output.
 * Loads persisted volume and mute state from NVS.
 * Returns 0 on success. */
int audio_init(void);

/* Deinitialize and release I2S resources */
void audio_deinit(void);

/* Play a predefined alert tone using current volume setting (non-blocking, queued) */
int audio_play_tone(audio_tone_t tone);

/* Play a raw tone: frequency in Hz, duration in ms.
 * Uses current volume setting; ignored if muted. */
int audio_play_beep(uint16_t freq_hz, uint16_t duration_ms);

/* Check if audio is currently playing */
bool audio_is_playing(void);

/* Check if audio hardware is available on this board */
bool audio_is_available(void);

/* Volume: 0–100.  Persisted to NVS immediately. */
void audio_set_volume(uint8_t volume);
uint8_t audio_get_volume(void);

/* Mute: when muted, all playback is suppressed but volume is retained.
 * Persisted to NVS immediately. */
void audio_set_muted(bool muted);
bool audio_get_muted(void);

#endif /* BRAMBLE_AUDIO_H */
