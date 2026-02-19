#include "audio.h"
#include "esp_log.h"

static const char *TAG = "audio";

int audio_init(void) {
    ESP_LOGI(TAG, "Audio subsystem: stub (not implemented)");
    return 0;
}

void audio_deinit(void) {
    /* stub */
}

int audio_record_start(void) {
    ESP_LOGW(TAG, "audio_record_start: not implemented");
    return -1;
}

int audio_record_stop(void) {
    ESP_LOGW(TAG, "audio_record_stop: not implemented");
    return -1;
}

int audio_play(const uint8_t *pcm, size_t len) {
    (void)pcm;
    (void)len;
    ESP_LOGW(TAG, "audio_play: not implemented");
    return -1;
}

int audio_is_available(void) {
    return 0;  /* Not available until implemented */
}
