#include "unity.h"

#include <stdbool.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

static esp_err_t g_https_ota_result;
static int g_https_ota_calls;
static esp_http_client_config_t g_last_https_http_cfg;

static const esp_partition_t g_running = {.label = "factory"};
static const esp_app_desc_t g_app_desc = {.version = "1.2.3"};

void* esp_crt_bundle_attach = (void*)0xCAFE;

esp_err_t esp_https_ota(const esp_https_ota_config_t* ota_config) {
    g_https_ota_calls++;
    g_last_https_http_cfg = *ota_config->http_config;
    return g_https_ota_result;
}

/* HTTP path is compile-time disabled in test build; keep minimal stubs anyway */
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    (void)config;
    return NULL;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len) {
    (void)client;
    (void)write_len;
    return ESP_FAIL;
}
int esp_http_client_fetch_headers(esp_http_client_handle_t client) {
    (void)client;
    return -1;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) {
    (void)client;
    return 500;
}
int esp_http_client_read(esp_http_client_handle_t client, char* buffer, int len) {
    (void)client;
    (void)buffer;
    (void)len;
    return -1;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client) {
    (void)client;
    return false;
}
void esp_http_client_close(esp_http_client_handle_t client) { (void)client; }
void esp_http_client_cleanup(esp_http_client_handle_t client) { (void)client; }

const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* start_from) {
    (void)start_from;
    return NULL;
}
esp_err_t esp_ota_begin(const esp_partition_t* partition, int image_size,
                        esp_ota_handle_t* out_handle) {
    (void)partition;
    (void)image_size;
    (void)out_handle;
    return ESP_FAIL;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* data, int size) {
    (void)handle;
    (void)data;
    (void)size;
    return ESP_FAIL;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle) {
    (void)handle;
    return ESP_FAIL;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition) {
    (void)partition;
    return ESP_FAIL;
}
esp_err_t esp_ota_abort(esp_ota_handle_t handle) {
    (void)handle;
    return ESP_FAIL;
}
const esp_partition_t* esp_ota_get_running_partition(void) { return &g_running; }
const char* esp_err_to_name(esp_err_t err) {
    (void)err;
    return "ERR";
}
const esp_app_desc_t* esp_app_get_description(void) { return &g_app_desc; }

#include "../components/ota/ota.c"

void setUp(void) {
    g_https_ota_result = ESP_OK;
    g_https_ota_calls = 0;
    memset(&g_last_https_http_cfg, 0, sizeof(g_last_https_http_cfg));
}

void tearDown(void) {}

void test_https_url_routes_to_https_ota_with_hardening_flags(void) {
    TEST_ASSERT_EQUAL_INT(0, ota_wifi_start("https://example.com/fw.bin"));
    TEST_ASSERT_EQUAL_INT(1, g_https_ota_calls);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", g_last_https_http_cfg.url);
    TEST_ASSERT_FALSE(g_last_https_http_cfg.skip_cert_common_name_check);
    TEST_ASSERT_EQUAL_PTR(esp_crt_bundle_attach, g_last_https_http_cfg.crt_bundle_attach);
}

void test_empty_and_unknown_scheme_are_rejected(void) {
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start(""));
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("ftp://example.com/fw.bin"));
    TEST_ASSERT_EQUAL_INT(0, g_https_ota_calls);
}

void test_http_is_rejected_when_allow_http_not_defined(void) {
#ifdef CONFIG_BRAMBLE_OTA_ALLOW_HTTP
    TEST_IGNORE_MESSAGE("Test requires CONFIG_BRAMBLE_OTA_ALLOW_HTTP to be undefined");
#endif
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("http://example.com/fw.bin"));
    TEST_ASSERT_EQUAL_INT(0, g_https_ota_calls);
}

void test_https_failure_propagates_error(void) {
    g_https_ota_result = ESP_FAIL;
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("https://example.com/fw.bin"));
    TEST_ASSERT_EQUAL_INT(1, g_https_ota_calls);
}

void test_partition_and_version_helpers(void) {
    TEST_ASSERT_EQUAL_STRING("factory", ota_get_running_partition());
    TEST_ASSERT_EQUAL_STRING("1.2.3", ota_get_app_version());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_https_url_routes_to_https_ota_with_hardening_flags);
    RUN_TEST(test_empty_and_unknown_scheme_are_rejected);
    RUN_TEST(test_http_is_rejected_when_allow_http_not_defined);
    RUN_TEST(test_https_failure_propagates_error);
    RUN_TEST(test_partition_and_version_helpers);
    return UNITY_END();
}
