#include "unity.h"

#include <stdbool.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "ota_rollback.h"

/* ── Controllable stubs for the advanced esp_https_ota API ─────────── */

static esp_err_t g_begin_result;
static esp_err_t g_img_desc_result;
static const char* g_img_desc_version;
static esp_err_t g_perform_result;
static bool g_complete_data;
static esp_err_t g_finish_result;
static int g_begin_calls;
static int g_finish_calls;
static int g_abort_calls;
static esp_http_client_config_t g_last_https_http_cfg;

static const esp_partition_t g_running = {.label = "factory"};
static const esp_app_desc_t g_app_desc = {.version = "1.2.3"};

void* esp_crt_bundle_attach = (void*)0xCAFE;

esp_err_t esp_https_ota(const esp_https_ota_config_t* ota_config) {
    (void)ota_config;
    return ESP_FAIL; /* legacy one-shot API must no longer be used */
}

esp_err_t esp_https_ota_begin(const esp_https_ota_config_t* ota_config,
                              esp_https_ota_handle_t* handle) {
    g_begin_calls++;
    g_last_https_http_cfg = *ota_config->http_config;
    *handle = (esp_https_ota_handle_t)0x1;
    return g_begin_result;
}
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle, esp_app_desc_t* out) {
    (void)handle;
    out->version = g_img_desc_version;
    return g_img_desc_result;
}
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle) {
    (void)handle;
    return g_perform_result;
}
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle) {
    (void)handle;
    return g_complete_data;
}
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle) {
    (void)handle;
    g_finish_calls++;
    return g_finish_result;
}
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle) {
    (void)handle;
    g_abort_calls++;
    return ESP_OK;
}
int esp_https_ota_get_image_size(esp_https_ota_handle_t handle) {
    (void)handle;
    return 0; /* Task 1 progress reporting; not asserted by these tests */
}
int esp_https_ota_get_image_len_read(esp_https_ota_handle_t handle) {
    (void)handle;
    return 0; /* Task 1 progress reporting; not asserted by these tests */
}

/* ── Anti-rollback gate stub (ota_rollback.c is device-only) ───────── */

static int g_gate_result;
static char g_gate_version[64];
static bool g_gate_allow_downgrade;
static int g_gate_calls;

int ota_rollback_gate(const char* new_version, bool allow_downgrade) {
    g_gate_calls++;
    snprintf(g_gate_version, sizeof(g_gate_version), "%s", new_version ? new_version : "");
    g_gate_allow_downgrade = allow_downgrade;
    return g_gate_result;
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
esp_err_t esp_ota_get_partition_description(const esp_partition_t* partition,
                                            esp_app_desc_t* app_desc) {
    (void)partition;
    (void)app_desc;
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
    g_begin_result = ESP_OK;
    g_img_desc_result = ESP_OK;
    g_img_desc_version = "1.3.0";
    g_perform_result = ESP_OK;
    g_complete_data = true;
    g_finish_result = ESP_OK;
    g_begin_calls = 0;
    g_finish_calls = 0;
    g_abort_calls = 0;
    g_gate_result = 0;
    g_gate_version[0] = '\0';
    g_gate_allow_downgrade = false;
    g_gate_calls = 0;
    memset(&g_last_https_http_cfg, 0, sizeof(g_last_https_http_cfg));
    s_last_error[0] = '\0';
}

void tearDown(void) {}

void test_https_url_routes_to_https_ota_with_hardening_flags(void) {
    TEST_ASSERT_EQUAL_INT(0, ota_wifi_start("https://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(1, g_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, g_finish_calls);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", g_last_https_http_cfg.url);
    TEST_ASSERT_FALSE(g_last_https_http_cfg.skip_cert_common_name_check);
    TEST_ASSERT_EQUAL_PTR(esp_crt_bundle_attach, g_last_https_http_cfg.crt_bundle_attach);
}

void test_empty_and_unknown_scheme_are_rejected(void) {
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("", false));
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("ftp://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(0, g_begin_calls);
}

void test_http_is_rejected_when_allow_http_not_defined(void) {
#ifdef CONFIG_BRAMBLE_OTA_ALLOW_HTTP
    TEST_IGNORE_MESSAGE("Test requires CONFIG_BRAMBLE_OTA_ALLOW_HTTP to be undefined");
#endif
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("http://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(0, g_begin_calls);
}

void test_rollback_gate_sees_image_version_and_downgrade_flag(void) {
    g_img_desc_version = "1.9.9";
    TEST_ASSERT_EQUAL_INT(0, ota_wifi_start("https://example.com/fw.bin", true));
    TEST_ASSERT_EQUAL_INT(1, g_gate_calls);
    TEST_ASSERT_EQUAL_STRING("1.9.9", g_gate_version);
    TEST_ASSERT_TRUE(g_gate_allow_downgrade);
}

void test_rollback_gate_rejection_aborts_before_download(void) {
    g_gate_result = -1;
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("https://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(1, g_abort_calls);
    TEST_ASSERT_EQUAL_INT(0, g_finish_calls);
    TEST_ASSERT_NOT_NULL(ota_get_last_error());
    TEST_ASSERT_NOT_NULL(strstr(ota_get_last_error(), "anti-rollback"));
}

void test_signature_validation_failure_fails_closed_with_clear_error(void) {
    g_finish_result = ESP_ERR_OTA_VALIDATE_FAILED;
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("https://example.com/fw.bin", false));
    TEST_ASSERT_NOT_NULL(ota_get_last_error());
    TEST_ASSERT_NOT_NULL(strstr(ota_get_last_error(), "signature"));
}

void test_incomplete_download_aborts(void) {
    g_complete_data = false;
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("https://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(1, g_abort_calls);
    TEST_ASSERT_EQUAL_INT(0, g_finish_calls);
}

void test_unreadable_image_description_aborts(void) {
    g_img_desc_result = ESP_FAIL;
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("https://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(1, g_abort_calls);
    TEST_ASSERT_EQUAL_INT(0, g_gate_calls);
}

void test_https_failure_propagates_error(void) {
    g_begin_result = ESP_FAIL;
    TEST_ASSERT_EQUAL_INT(-1, ota_wifi_start("https://example.com/fw.bin", false));
    TEST_ASSERT_EQUAL_INT(1, g_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, g_finish_calls);
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
    RUN_TEST(test_rollback_gate_sees_image_version_and_downgrade_flag);
    RUN_TEST(test_rollback_gate_rejection_aborts_before_download);
    RUN_TEST(test_signature_validation_failure_fails_closed_with_clear_error);
    RUN_TEST(test_incomplete_download_aborts);
    RUN_TEST(test_unreadable_image_description_aborts);
    RUN_TEST(test_https_failure_propagates_error);
    RUN_TEST(test_partition_and_version_helpers);
    return UNITY_END();
}
