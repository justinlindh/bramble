#include "audio.h"
#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <math.h>
#include <string.h>

static const char *TAG = "audio";

/* ── Constants ──────────────────────────────────────────────────────── */

#define SAMPLE_RATE      16000
#define BUFFER_SAMPLES   1024
#define QUEUE_DEPTH      4

#define DEFAULT_VOLUME   50      /* Sane default — not screaming loud */
#define NVS_NAMESPACE    "bramble"
#define NVS_KEY_VOLUME   "audio_vol"
#define NVS_KEY_MUTED    "audio_mute"

/* ── Tone definitions ───────────────────────────────────────────────── */

typedef struct {
    uint16_t freq;  /* Hz */
    uint16_t dur;   /* ms */
    uint16_t gap;   /* silence after note, ms */
} tone_note_t;

static const tone_note_t tone_message_rx[]  = {{880, 80, 60}, {880, 80, 0}};
static const tone_note_t tone_message_tx[]  = {{660, 60, 0}, {880, 100, 0}};
static const tone_note_t tone_peer_join[]   = {{523, 80, 30}, {659, 80, 30}, {784, 120, 0}};
static const tone_note_t tone_peer_leave[]  = {{784, 80, 30}, {659, 80, 30}, {523, 120, 0}};
static const tone_note_t tone_error[]       = {{200, 200, 100}, {200, 200, 0}};
static const tone_note_t tone_boot[]        = {{523, 60, 20}, {659, 60, 20}, {784, 60, 20}, {1047, 120, 0}};
static const tone_note_t tone_gps_fix[]     = {{1000, 60, 40}, {1000, 60, 0}};

typedef struct {
    const tone_note_t *notes;
    size_t             count;
} tone_sequence_t;

/* ── Audio state ────────────────────────────────────────────────────── */

static struct {
    i2s_chan_handle_t  tx_chan;
    QueueHandle_t      tone_queue;
    TaskHandle_t       task_handle;
    bool               initialized;
    bool               muted;
    uint8_t            volume;   /* 0–100 */
    bool               playing;
} s_audio = {0};

/* ── NVS helpers ────────────────────────────────────────────────────── */

static void nvs_load_prefs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        s_audio.volume = DEFAULT_VOLUME;
        s_audio.muted  = false;
        return;
    }
    uint8_t vol  = DEFAULT_VOLUME;
    uint8_t mute = 0;
    nvs_get_u8(h, NVS_KEY_VOLUME, &vol);
    nvs_get_u8(h, NVS_KEY_MUTED,  &mute);
    nvs_close(h);
    s_audio.volume = (vol > 100) ? 100 : vol;
    s_audio.muted  = (mute != 0);
    ESP_LOGI(TAG, "Audio prefs loaded: volume=%u muted=%d", s_audio.volume, s_audio.muted);
}

static void nvs_save_volume(uint8_t vol) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_VOLUME, vol);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_muted(bool muted) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_MUTED, (uint8_t)muted);
    nvs_commit(h);
    nvs_close(h);
}

/* ── Internal playback helpers ──────────────────────────────────────── */

static void play_beep_internal(uint16_t freq_hz, uint16_t duration_ms) {
    if (!s_audio.initialized || s_audio.muted || s_audio.volume == 0) {
        return;
    }

    size_t  total_samples = (SAMPLE_RATE * duration_ms) / 1000;
    int16_t buffer[BUFFER_SAMPLES];
    size_t  written = 0;

    /* Apply a mild perceptual curve: linear in amplitude feels too loud at
     * the top end.  Square-root gives a more natural perceived loudness. */
    float vol_scale = sqrtf(s_audio.volume / 100.0f);

    s_audio.playing = true;

    while (written < total_samples) {
        size_t chunk = (total_samples - written > BUFFER_SAMPLES)
                        ? BUFFER_SAMPLES
                        : (total_samples - written);

        for (size_t i = 0; i < chunk; i++) {
            float t = (float)(written + i) / SAMPLE_RATE;
            buffer[i] = (int16_t)(vol_scale * 16000.0f *
                                  sinf(2.0f * (float)M_PI * freq_hz * t));
        }

        size_t bytes_written;
        i2s_channel_write(s_audio.tx_chan, buffer, chunk * sizeof(int16_t),
                          &bytes_written, portMAX_DELAY);
        written += chunk;
    }

    s_audio.playing = false;
}

static void play_silence(uint16_t duration_ms) {
    if (!s_audio.initialized || duration_ms == 0) return;

    size_t  total_samples = (SAMPLE_RATE * duration_ms) / 1000;
    int16_t buffer[BUFFER_SAMPLES];
    memset(buffer, 0, sizeof(buffer));
    size_t written = 0;

    while (written < total_samples) {
        size_t chunk = (total_samples - written > BUFFER_SAMPLES)
                        ? BUFFER_SAMPLES
                        : (total_samples - written);
        size_t bytes_written;
        i2s_channel_write(s_audio.tx_chan, buffer, chunk * sizeof(int16_t),
                          &bytes_written, portMAX_DELAY);
        written += chunk;
    }
}

/* ── Audio task ─────────────────────────────────────────────────────── */

static void audio_task(void *arg) {
    tone_sequence_t seq;
    while (1) {
        if (xQueueReceive(s_audio.tone_queue, &seq, portMAX_DELAY) == pdTRUE) {
            for (size_t i = 0; i < seq.count; i++) {
                const tone_note_t *note = &seq.notes[i];
                play_beep_internal(note->freq, note->dur);
                if (note->gap > 0) {
                    play_silence(note->gap);
                }
            }
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

int audio_init(void) {
    const bramble_board_config_t *cfg = board_get_config();

    if (!(cfg->capabilities & BOARD_CAP_AUDIO)) {
        ESP_LOGI(TAG, "Audio not available on this board");
        return -1;
    }

    if (s_audio.initialized) {
        ESP_LOGW(TAG, "Audio already initialized");
        return 0;
    }

    /* Restore persisted preferences before anything plays */
    nvs_load_prefs();

    ESP_LOGI(TAG, "Initializing I2S audio (MAX98357A) — volume=%u muted=%d",
             s_audio.volume, s_audio.muted);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_audio.tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(err));
        return -1;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk  = I2S_GPIO_UNUSED,
            .bclk  = cfg->audio.i2s_bck,
            .ws    = cfg->audio.i2s_ws,
            .dout  = cfg->audio.i2s_dout,
            .din   = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_audio.tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S std mode init failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_audio.tx_chan);
        return -1;
    }

    err = i2s_channel_enable(s_audio.tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_audio.tx_chan);
        return -1;
    }

    s_audio.tone_queue = xQueueCreate(QUEUE_DEPTH, sizeof(tone_sequence_t));
    if (!s_audio.tone_queue) {
        ESP_LOGE(TAG, "Tone queue create failed");
        i2s_channel_disable(s_audio.tx_chan);
        i2s_del_channel(s_audio.tx_chan);
        return -1;
    }

    BaseType_t ret = xTaskCreate(audio_task, "audio", 4096, NULL, 3,
                                 &s_audio.task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Audio task create failed");
        vQueueDelete(s_audio.tone_queue);
        i2s_channel_disable(s_audio.tx_chan);
        i2s_del_channel(s_audio.tx_chan);
        return -1;
    }

    s_audio.initialized = true;
    s_audio.playing     = false;

    ESP_LOGI(TAG, "Audio ready: WS=%d BCK=%d DOUT=%d",
             cfg->audio.i2s_ws, cfg->audio.i2s_bck, cfg->audio.i2s_dout);
    return 0;
}

void audio_deinit(void) {
    if (!s_audio.initialized) return;

    if (s_audio.task_handle) {
        vTaskDelete(s_audio.task_handle);
        s_audio.task_handle = NULL;
    }
    if (s_audio.tone_queue) {
        vQueueDelete(s_audio.tone_queue);
        s_audio.tone_queue = NULL;
    }
    if (s_audio.tx_chan) {
        i2s_channel_disable(s_audio.tx_chan);
        i2s_del_channel(s_audio.tx_chan);
        s_audio.tx_chan = NULL;
    }
    s_audio.initialized = false;
    ESP_LOGI(TAG, "Audio deinitialized");
}

int audio_play_tone(audio_tone_t tone) {
    if (!s_audio.initialized) return -1;

    tone_sequence_t seq;
    switch (tone) {
        case AUDIO_TONE_MESSAGE_RX:
            seq.notes = tone_message_rx;
            seq.count = sizeof(tone_message_rx) / sizeof(tone_note_t); break;
        case AUDIO_TONE_MESSAGE_TX:
            seq.notes = tone_message_tx;
            seq.count = sizeof(tone_message_tx) / sizeof(tone_note_t); break;
        case AUDIO_TONE_PEER_JOIN:
            seq.notes = tone_peer_join;
            seq.count = sizeof(tone_peer_join)  / sizeof(tone_note_t); break;
        case AUDIO_TONE_PEER_LEAVE:
            seq.notes = tone_peer_leave;
            seq.count = sizeof(tone_peer_leave) / sizeof(tone_note_t); break;
        case AUDIO_TONE_ERROR:
            seq.notes = tone_error;
            seq.count = sizeof(tone_error)      / sizeof(tone_note_t); break;
        case AUDIO_TONE_BOOT:
            seq.notes = tone_boot;
            seq.count = sizeof(tone_boot)       / sizeof(tone_note_t); break;
        case AUDIO_TONE_GPS_FIX:
            seq.notes = tone_gps_fix;
            seq.count = sizeof(tone_gps_fix)    / sizeof(tone_note_t); break;
        default:
            ESP_LOGW(TAG, "Unknown tone %d", tone);
            return -1;
    }

    if (xQueueSend(s_audio.tone_queue, &seq, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Tone queue full");
        return -1;
    }
    return 0;
}

int audio_play_beep(uint16_t freq_hz, uint16_t duration_ms) {
    if (!s_audio.initialized) return -1;

    static tone_note_t beep_note;
    beep_note = (tone_note_t){ .freq = freq_hz, .dur = duration_ms, .gap = 0 };

    tone_sequence_t seq = { .notes = &beep_note, .count = 1 };
    if (xQueueSend(s_audio.tone_queue, &seq, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Tone queue full");
        return -1;
    }
    return 0;
}

bool audio_is_playing(void) {
    return s_audio.playing;
}

bool audio_is_available(void) {
    return (board_get_config()->capabilities & BOARD_CAP_AUDIO) != 0;
}

void audio_set_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    s_audio.volume = volume;
    nvs_save_volume(volume);
    ESP_LOGI(TAG, "Volume set to %u", volume);
}

uint8_t audio_get_volume(void) {
    return s_audio.volume;
}

void audio_set_muted(bool muted) {
    s_audio.muted = muted;
    nvs_save_muted(muted);
    ESP_LOGI(TAG, "Audio %s", muted ? "muted" : "unmuted");
}

bool audio_get_muted(void) {
    return s_audio.muted;
}
