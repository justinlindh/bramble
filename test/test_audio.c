#include "unity.h"
#include "freertos/task.h"
#include "audio.h"
#include "board_config.h"
#include "nvs.h"
#include "driver/i2s_std.h"
#include "freertos/queue.h"
#include <string.h>

/* ---- board stub ---- */
static bramble_board_config_t g_cfg;
const bramble_board_config_t* board_get_config(void) { return &g_cfg; }

/* ---- NVS stub ---- */
static int g_nvs_open_ok = 1;
static uint8_t g_nvs_vol = 50;
static uint8_t g_nvs_mute = 0;
static int g_nvs_has_vol = 0;
static int g_nvs_has_mute = 0;

esp_err_t nvs_open(const char* ns, int mode, nvs_handle_t* out) {
    (void)ns;
    (void)mode;
    if (!g_nvs_open_ok)
        return ESP_FAIL;
    *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char* key, uint8_t* out) {
    (void)h;
    if (strcmp(key, "audio_vol") == 0 && g_nvs_has_vol) {
        *out = g_nvs_vol;
        return ESP_OK;
    }
    if (strcmp(key, "audio_mute") == 0 && g_nvs_has_mute) {
        *out = g_nvs_mute;
        return ESP_OK;
    }
    return ESP_FAIL;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char* key, uint8_t value) {
    (void)h;
    if (strcmp(key, "audio_vol") == 0) {
        g_nvs_vol = value;
        g_nvs_has_vol = 1;
        return ESP_OK;
    }
    if (strcmp(key, "audio_mute") == 0) {
        g_nvs_mute = value;
        g_nvs_has_mute = 1;
        return ESP_OK;
    }
    return ESP_FAIL;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    return ESP_OK;
}

/* unused nvs API for link */
esp_err_t nvs_get_str(nvs_handle_t h, const char* key, char* out_value, size_t* length) {
    (void)h;
    (void)key;
    (void)out_value;
    (void)length;
    return ESP_FAIL;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char* key, const char* value) {
    (void)h;
    (void)key;
    (void)value;
    return ESP_FAIL;
}
esp_err_t nvs_set_u16(nvs_handle_t h, const char* key, uint16_t value) {
    (void)h;
    (void)key;
    (void)value;
    return ESP_FAIL;
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char* key, uint32_t value) {
    (void)h;
    (void)key;
    (void)value;
    return ESP_FAIL;
}
esp_err_t nvs_set_i8(nvs_handle_t h, const char* key, int8_t value) {
    (void)h;
    (void)key;
    (void)value;
    return ESP_FAIL;
}
esp_err_t nvs_set_i32(nvs_handle_t h, const char* key, int32_t value) {
    (void)h;
    (void)key;
    (void)value;
    return ESP_FAIL;
}
esp_err_t nvs_get_u16(nvs_handle_t h, const char* key, uint16_t* out) {
    (void)h;
    (void)key;
    (void)out;
    return ESP_FAIL;
}
esp_err_t nvs_get_i32(nvs_handle_t h, const char* key, int32_t* out) {
    (void)h;
    (void)key;
    (void)out;
    return ESP_FAIL;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out_value, size_t* length) {
    (void)h;
    (void)key;
    (void)out_value;
    (void)length;
    return ESP_FAIL;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char* key) {
    (void)h;
    (void)key;
    return ESP_FAIL;
}
esp_err_t nvs_entry_find(const char* part_name, const char* namespace_name, nvs_type_t type,
                         nvs_iterator_t* out_iterator) {
    (void)part_name;
    (void)namespace_name;
    (void)type;
    (void)out_iterator;
    return ESP_FAIL;
}
esp_err_t nvs_entry_next(nvs_iterator_t* iterator) {
    (void)iterator;
    return ESP_FAIL;
}
void nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t* out_info) {
    (void)iterator;
    (void)out_info;
}
void nvs_release_iterator(nvs_iterator_t iterator) { (void)iterator; }

/* ---- I2S/RTOS stubs ---- */
static int g_i2s_new_channel_calls, g_i2s_enable_calls, g_i2s_disable_calls, g_i2s_delete_calls;
static int g_queue_create_calls, g_queue_delete_calls, g_task_create_calls, g_task_delete_calls;
static int g_queue_send_calls;

esp_err_t i2s_new_channel(const i2s_chan_config_t* chan_cfg, i2s_chan_handle_t* tx_handle,
                          i2s_chan_handle_t* rx_handle) {
    (void)chan_cfg;
    (void)rx_handle;
    g_i2s_new_channel_calls++;
    *tx_handle = (void*)0x1;
    return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t tx_handle, const i2s_std_config_t* std_cfg) {
    (void)tx_handle;
    (void)std_cfg;
    return ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t tx_handle) {
    (void)tx_handle;
    g_i2s_enable_calls++;
    return ESP_OK;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void* src, size_t size,
                            size_t* bytes_written, uint32_t timeout_ms) {
    (void)handle;
    (void)src;
    (void)timeout_ms;
    if (bytes_written)
        *bytes_written = size;
    return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t tx_handle) {
    (void)tx_handle;
    g_i2s_disable_calls++;
    return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t tx_handle) {
    (void)tx_handle;
    g_i2s_delete_calls++;
    return ESP_OK;
}
const char* esp_err_to_name(esp_err_t err) {
    (void)err;
    return "ERR";
}

QueueHandle_t xQueueCreate(unsigned int queue_length, unsigned int item_size) {
    (void)queue_length;
    (void)item_size;
    g_queue_create_calls++;
    return (void*)0x2;
}
void vQueueDelete(QueueHandle_t queue) {
    (void)queue;
    g_queue_delete_calls++;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, unsigned int ticks_to_wait) {
    (void)queue;
    (void)item;
    (void)ticks_to_wait;
    g_queue_send_calls++;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void* buffer, unsigned int ticks_to_wait) {
    (void)queue;
    (void)buffer;
    (void)ticks_to_wait;
    return pdFALSE;
}

int xTaskCreate(void (*task)(void*), const char* name, int stack, void* arg, int pri,
                TaskHandle_t* out) {
    (void)task;
    (void)name;
    (void)stack;
    (void)arg;
    (void)pri;
    g_task_create_calls++;
    if (out)
        *out = (void*)0x3;
    return 1;
}
void vTaskDelete(TaskHandle_t t) {
    (void)t;
    g_task_delete_calls++;
}

static void reset_state(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.capabilities = BOARD_CAP_AUDIO;
    g_cfg.audio.i2s_ws = 1;
    g_cfg.audio.i2s_bck = 2;
    g_cfg.audio.i2s_dout = 3;
    g_nvs_open_ok = 1;
    g_nvs_vol = 50;
    g_nvs_mute = 0;
    g_nvs_has_vol = 0;
    g_nvs_has_mute = 0;
    g_i2s_new_channel_calls = g_i2s_enable_calls = g_i2s_disable_calls = g_i2s_delete_calls = 0;
    g_queue_create_calls = g_queue_delete_calls = g_task_create_calls = g_task_delete_calls = 0;
    g_queue_send_calls = 0;
}

void setUp(void) { reset_state(); }
void tearDown(void) { audio_deinit(); }

void test_audio_init_loads_nvs_preferences(void) {
    g_nvs_has_vol = 1;
    g_nvs_vol = 77;
    g_nvs_has_mute = 1;
    g_nvs_mute = 1;
    TEST_ASSERT_EQUAL(0, audio_init());
    TEST_ASSERT_EQUAL_UINT8(77, audio_get_volume());
    TEST_ASSERT_TRUE(audio_get_muted());
}

void test_audio_volume_and_mute_persist_to_nvs(void) {
    TEST_ASSERT_EQUAL(0, audio_init());
    audio_set_volume(150);
    audio_set_muted(true);
    TEST_ASSERT_EQUAL_UINT8(100, audio_get_volume());
    TEST_ASSERT_TRUE(audio_get_muted());
    TEST_ASSERT_EQUAL_UINT8(100, g_nvs_vol);
    TEST_ASSERT_EQUAL_UINT8(1, g_nvs_mute);
}

void test_audio_tone_lookup_and_queueing(void) {
    TEST_ASSERT_EQUAL(0, audio_init());
    TEST_ASSERT_EQUAL(0, audio_play_tone(AUDIO_TONE_MESSAGE_RX));
    TEST_ASSERT_EQUAL(0, audio_play_tone(AUDIO_TONE_GPS_FIX));
    TEST_ASSERT_EQUAL(-1, audio_play_tone((audio_tone_t)99));
    TEST_ASSERT_EQUAL(2, g_queue_send_calls);
}

void test_audio_init_deinit_lifecycle_calls_i2s_and_tasks(void) {
    TEST_ASSERT_EQUAL(0, audio_init());
    TEST_ASSERT_EQUAL(1, g_i2s_new_channel_calls);
    TEST_ASSERT_EQUAL(1, g_i2s_enable_calls);
    TEST_ASSERT_EQUAL(1, g_queue_create_calls);
    TEST_ASSERT_EQUAL(1, g_task_create_calls);

    audio_deinit();
    TEST_ASSERT_EQUAL(1, g_task_delete_calls);
    TEST_ASSERT_EQUAL(1, g_queue_delete_calls);
    TEST_ASSERT_EQUAL(1, g_i2s_disable_calls);
    TEST_ASSERT_EQUAL(1, g_i2s_delete_calls);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_audio_init_loads_nvs_preferences);
    RUN_TEST(test_audio_volume_and_mute_persist_to_nvs);
    RUN_TEST(test_audio_tone_lookup_and_queueing);
    RUN_TEST(test_audio_init_deinit_lifecycle_calls_i2s_and_tasks);
    return UNITY_END();
}
