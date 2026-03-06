/* ESP-IDF stubs for host compilation */
#include "esp_stubs.h"

/*
 * Default NVS stubs — simple no-ops that return ESP_OK / ESP_FAIL.
 * Tests that need custom NVS behavior (e.g. test_audio) define their own
 * implementations and should NOT link this file's NVS symbols (they include
 * inline stubs and don't link esp_stubs.c, or use weak linkage).
 *
 * Guarded by NVS_STUBS_ENABLE so only tests that opt in get these symbols,
 * avoiding duplicate definition conflicts with test_audio's custom stubs.
 */
#ifdef NVS_STUBS_ENABLE
#include "nvs.h"

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out) {
    (void)ns; (void)mode;
    if (out) *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) {
    (void)h; (void)key;
    if (out) *out = 0;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val) {
    (void)h; (void)key; (void)val;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) {
    (void)h; (void)key; (void)out; (void)len; return ESP_FAIL;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val) {
    (void)h; (void)key; (void)val; return ESP_FAIL;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {
    (void)h; (void)key; (void)out; (void)len; return ESP_FAIL;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h; (void)key; return ESP_FAIL;
}
#endif /* NVS_STUBS_ENABLE */
