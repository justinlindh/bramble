/* Tell the wifi_manager.h stub to provide declarations only (no inline impls) */
#define WIFI_MANAGER_TEST

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "wifi_ap_password.h"

/* ---- controllable test doubles ---- */

const char* WIFI_EVENT = "WIFI_EVENT";
const char* IP_EVENT = "IP_EVENT";

static esp_err_t g_nvs_open_err;
static esp_err_t g_nvs_get_ssid_err;
static esp_err_t g_nvs_get_pass_err;
static char g_nvs_ssid[33];
static char g_nvs_pass[65];
static char g_saved_ssid[33];
static char g_saved_pass[65];
static int g_nvs_commit_calls;
static int g_nvs_erase_calls;

static EventBits_t g_wait_bits_result;
static EventBits_t g_set_bits_accum;

static int g_wifi_set_mode;
static int g_wifi_connect_calls;
static wifi_config_t g_last_wifi_cfg;

static int g_register_calls;
static int g_unregister_calls;
static void (*g_sta_handler)(void*, esp_event_base_t, int32_t, void*);

static int g_ws_start_calls;
static int g_ws_stop_calls;
static bool g_ws_running;

static esp_netif_t g_sta_netif;
static esp_netif_t g_ap_netif;
static int g_create_sta_calls;
static int g_create_ap_calls;

esp_err_t nvs_open(const char* ns, int mode, nvs_handle_t* out_handle) {
    (void)mode;
    if (strcmp(ns, "bramble_wifi") != 0) {
        return ESP_FAIL;
    }
    if (g_nvs_open_err != ESP_OK) {
        return g_nvs_open_err;
    }
    if (out_handle) {
        *out_handle = 7;
    }
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out_value, size_t* length) {
    (void)handle;
    if (strcmp(key, "ssid") == 0) {
        if (g_nvs_get_ssid_err != ESP_OK) {
            return g_nvs_get_ssid_err;
        }
        if (out_value && length) {
            strncpy(out_value, g_nvs_ssid, *length - 1);
            out_value[*length - 1] = '\0';
        }
        return ESP_OK;
    }
    if (strcmp(key, "password") == 0) {
        if (g_nvs_get_pass_err != ESP_OK) {
            return g_nvs_get_pass_err;
        }
        if (out_value && length) {
            strncpy(out_value, g_nvs_pass, *length - 1);
            out_value[*length - 1] = '\0';
        }
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value) {
    (void)handle;
    if (strcmp(key, "ssid") == 0) {
        strncpy(g_saved_ssid, value, sizeof(g_saved_ssid) - 1);
        g_saved_ssid[sizeof(g_saved_ssid) - 1] = '\0';
    } else if (strcmp(key, "password") == 0) {
        strncpy(g_saved_pass, value, sizeof(g_saved_pass) - 1);
        g_saved_pass[sizeof(g_saved_pass) - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key) {
    (void)handle;
    (void)key;
    g_nvs_erase_calls++;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    (void)handle;
    g_nvs_commit_calls++;
    return ESP_OK;
}

EventGroupHandle_t xEventGroupCreate(void) { return (EventGroupHandle_t)0x1; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToWaitFor,
                                const int xClearOnExit, const int xWaitForAllBits,
                                TickType_t xTicksToWait) {
    (void)xEventGroup;
    (void)uxBitsToWaitFor;
    (void)xClearOnExit;
    (void)xWaitForAllBits;
    (void)xTicksToWait;
    return g_wait_bits_result;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToSet) {
    (void)xEventGroup;
    g_set_bits_accum |= uxBitsToSet;
    return g_set_bits_accum;
}
void vEventGroupDelete(EventGroupHandle_t xEventGroup) { (void)xEventGroup; }

esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_netif_t* esp_netif_create_default_wifi_sta(void) {
    g_create_sta_calls++;
    return &g_sta_netif;
}
esp_netif_t* esp_netif_create_default_wifi_ap(void) {
    g_create_ap_calls++;
    return &g_ap_netif;
}
void esp_netif_destroy(esp_netif_t* netif) { (void)netif; }
esp_err_t esp_netif_set_ip_info(esp_netif_t* netif, const esp_netif_ip_info_t* ip_info) {
    (void)netif;
    (void)ip_info;
    return ESP_OK;
}
esp_err_t esp_netif_dhcps_stop(esp_netif_t* netif) {
    (void)netif;
    return ESP_OK;
}
esp_err_t esp_netif_dhcps_start(esp_netif_t* netif) {
    (void)netif;
    return ESP_OK;
}

esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t event_base, int32_t event_id,
                                              void (*event_handler)(void*, esp_event_base_t,
                                                                    int32_t, void*),
                                              void* event_handler_arg,
                                              esp_event_handler_instance_t* instance) {
    (void)event_base;
    (void)event_id;
    (void)event_handler_arg;
    g_register_calls++;
    g_sta_handler = event_handler;
    if (instance) {
        *instance = g_register_calls;
    }
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t event_base, int32_t event_id,
                                                esp_event_handler_instance_t instance) {
    (void)event_base;
    (void)event_id;
    (void)instance;
    g_unregister_calls++;
    return ESP_OK;
}

esp_err_t esp_wifi_init(const wifi_init_config_t* config) {
    (void)config;
    return ESP_OK;
}
esp_err_t esp_wifi_set_mode(wifi_mode_t mode) {
    g_wifi_set_mode = mode;
    return ESP_OK;
}
esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t* conf) {
    (void)interface;
    g_last_wifi_cfg = *conf;
    return ESP_OK;
}
esp_err_t esp_wifi_start(void) { return ESP_OK; }
esp_err_t esp_wifi_stop(void) { return ESP_OK; }
esp_err_t esp_wifi_deinit(void) { return ESP_OK; }
esp_err_t esp_wifi_connect(void) {
    g_wifi_connect_calls++;
    return ESP_OK;
}
esp_err_t esp_wifi_get_mac(int ifx, uint8_t mac[6]) {
    (void)ifx;
    memset(mac, 0, 6);
    return ESP_OK;
}
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t* list) {
    if (list) {
        memset(list, 0, sizeof(*list));
    }
    return ESP_OK;
}

/* esp_log_level_set is provided by the esp_log.h stub as static inline;
   wifi_manager.c will use that version directly. */

int ws_server_start(void) {
    g_ws_start_calls++;
    g_ws_running = true;
    return 0;
}
void ws_server_stop(void) {
    g_ws_stop_calls++;
    g_ws_running = false;
}
bool ws_server_is_running(void) { return g_ws_running; }

#include "../components/wifi/wifi_manager.c"

void setUp(void) {
    g_nvs_open_err = ESP_OK;
    g_nvs_get_ssid_err = ESP_OK;
    g_nvs_get_pass_err = ESP_OK;
    g_nvs_ssid[0] = '\0';
    g_nvs_pass[0] = '\0';
    g_saved_ssid[0] = '\0';
    g_saved_pass[0] = '\0';
    g_nvs_commit_calls = 0;
    g_nvs_erase_calls = 0;
    g_wait_bits_result = 0;
    g_set_bits_accum = 0;
    g_wifi_set_mode = 0;
    g_wifi_connect_calls = 0;
    memset(&g_last_wifi_cfg, 0, sizeof(g_last_wifi_cfg));
    g_register_calls = 0;
    g_unregister_calls = 0;
    g_sta_handler = NULL;
    g_ws_start_calls = 0;
    g_ws_stop_calls = 0;
    g_ws_running = false;
    g_create_sta_calls = 0;
    g_create_ap_calls = 0;

    /* Reset wifi_manager.c internal statics so each test starts clean */
    s_wifi_event_group = NULL;
    s_sta_handlers_registered = false;
    s_sta_any_id = 0;
    s_sta_got_ip = 0;
    memset(&s_status, 0, sizeof(s_status));
    memset(s_ap_secret, 0, sizeof(s_ap_secret));
    s_ap_secret_len = 0;
}

/* Synthetic identity secret. Never a real device's key, so no password this
 * suite computes belongs to any real node. */
static void fake_secret(uint8_t out[64], uint8_t seed) {
    for (int i = 0; i < 64; i++) {
        out[i] = (uint8_t)(seed * 31u + (unsigned)i * 7u + 11u);
    }
}

void tearDown(void) {}

void test_nvs_roundtrip_and_clear(void) {
    TEST_ASSERT_EQUAL_INT(0, wifi_manager_nvs_set_creds("MyNet", "secret"));
    TEST_ASSERT_EQUAL_STRING("MyNet", g_saved_ssid);
    TEST_ASSERT_EQUAL_STRING("secret", g_saved_pass);
    TEST_ASSERT_EQUAL_INT(1, g_nvs_commit_calls);

    strcpy(g_nvs_ssid, "MyNet");
    strcpy(g_nvs_pass, "secret");

    char ssid[33] = {0};
    char pass[65] = {0};
    TEST_ASSERT_EQUAL_INT(0, wifi_manager_nvs_get_creds(ssid, sizeof(ssid), pass, sizeof(pass)));
    TEST_ASSERT_EQUAL_STRING("MyNet", ssid);
    TEST_ASSERT_EQUAL_STRING("secret", pass);

    TEST_ASSERT_EQUAL_INT(0, wifi_manager_nvs_clear_creds());
    TEST_ASSERT_EQUAL_INT(2, g_nvs_erase_calls);
}

void test_nvs_get_creds_treats_missing_password_as_open_network(void) {
    strcpy(g_nvs_ssid, "OpenAP");
    g_nvs_get_pass_err = ESP_FAIL;

    char ssid[33] = {0};
    char pass[65] = "x";
    TEST_ASSERT_EQUAL_INT(0, wifi_manager_nvs_get_creds(ssid, sizeof(ssid), pass, sizeof(pass)));
    TEST_ASSERT_EQUAL_STRING("OpenAP", ssid);
    TEST_ASSERT_EQUAL_STRING("", pass);
}

void test_init_prefers_station_with_saved_creds(void) {
    strcpy(g_nvs_ssid, "SavedNet");
    strcpy(g_nvs_pass, "savedpass");
    g_wait_bits_result = BIT0;

    TEST_ASSERT_EQUAL_INT(0, wifi_manager_init(0x1A2B));
    TEST_ASSERT_EQUAL_INT(WIFI_MODE_STA, g_wifi_set_mode);
    TEST_ASSERT_EQUAL_STRING("SavedNet", (char*)g_last_wifi_cfg.sta.ssid);
    TEST_ASSERT_EQUAL_INT(0, g_create_ap_calls);

    /* In production, IP_EVENT_STA_GOT_IP fires before xEventGroupWaitBits returns BIT0.
     * The mock returns BIT0 directly; fire the event explicitly to set status.mode. */
    TEST_ASSERT_NOT_NULL(g_sta_handler);
    ip_event_got_ip_t got_ip = {0};
    got_ip.ip_info.ip.addr = ESP_IP4TOADDR(192, 168, 1, 100);
    g_sta_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip);

    wifi_status_t status = {0};
    wifi_manager_get_status(&status);
    TEST_ASSERT_EQUAL_INT(BRAMBLE_WIFI_STATION, status.mode);
    TEST_ASSERT_EQUAL_STRING("SavedNet", status.ssid);
}

void test_init_falls_back_to_ap_when_station_fails(void) {
    g_nvs_open_err = ESP_FAIL;
    g_wait_bits_result = BIT1;

    uint8_t secret[64];
    fake_secret(secret, 42);
    wifi_manager_set_ap_secret(secret, sizeof(secret));

    TEST_ASSERT_EQUAL_INT(0, wifi_manager_init(0xEC7A));
    TEST_ASSERT_EQUAL_INT(WIFI_MODE_AP, g_wifi_set_mode);
    TEST_ASSERT_TRUE(g_create_ap_calls > 0);

    wifi_status_t status = {0};
    wifi_manager_get_status(&status);
    TEST_ASSERT_EQUAL_INT(BRAMBLE_WIFI_AP, status.mode);
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", status.ip_addr);
    TEST_ASSERT_EQUAL_STRING("Bramble-EC7A", status.ssid);
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", wifi_manager_get_ip());

    /* The AP came up on the derived password, and the status surface carries
     * it so the on-device UIs and the serial CLI can show it. */
    char expect[WIFI_AP_PASSWORD_BUFSZ] = {0};
    TEST_ASSERT_EQUAL_INT(0,
                          wifi_ap_password_derive(secret, sizeof(secret), expect, sizeof(expect)));
    TEST_ASSERT_EQUAL_STRING(expect, status.ap_password);
    TEST_ASSERT_EQUAL_STRING(expect, (char*)g_last_wifi_cfg.ap.password);
    TEST_ASSERT_EQUAL_INT(WIFI_AUTH_WPA2_PSK, g_last_wifi_cfg.ap.authmode);
}

void test_ap_mode_refuses_to_start_without_a_password(void) {
    /* No secret provisioned and no build-time override: the node must not
     * fall back to a guessable or open AP, it must not come up at all. */
    g_nvs_open_err = ESP_FAIL;
    g_wait_bits_result = BIT1;

    TEST_ASSERT_EQUAL_INT(-1, wifi_manager_init(0xEC7A));

    wifi_status_t status = {0};
    wifi_manager_get_status(&status);
    TEST_ASSERT_NOT_EQUAL(BRAMBLE_WIFI_AP, status.mode);
    TEST_ASSERT_EQUAL_STRING("", status.ap_password);
}

void test_ap_password_is_stable_across_reinit(void) {
    uint8_t secret[64];
    fake_secret(secret, 7);

    char first[WIFI_AP_PASSWORD_FIELD] = {0};
    char second[WIFI_AP_PASSWORD_FIELD] = {0};

    wifi_manager_set_ap_secret(secret, sizeof(secret));
    TEST_ASSERT_EQUAL_INT(0, wifi_manager_get_ap_password(first, sizeof(first)));

    /* Same secret after a "reboot" (re-provision) yields the same password:
     * a user who wrote it down keeps a working credential. */
    memset(s_ap_secret, 0, sizeof(s_ap_secret));
    s_ap_secret_len = 0;
    wifi_manager_set_ap_secret(secret, sizeof(secret));
    TEST_ASSERT_EQUAL_INT(0, wifi_manager_get_ap_password(second, sizeof(second)));

    TEST_ASSERT_EQUAL_STRING(first, second);
    TEST_ASSERT_EQUAL_UINT(WIFI_AP_PASSWORD_LEN, (unsigned)strlen(first));
}

void test_get_ap_password_fails_closed_with_no_secret(void) {
    char pw[WIFI_AP_PASSWORD_FIELD];
    memset(pw, 'x', sizeof(pw));
    TEST_ASSERT_EQUAL_INT(-1, wifi_manager_get_ap_password(pw, sizeof(pw)));
    TEST_ASSERT_EQUAL_STRING("", pw);

    /* An oversized secret is rejected outright rather than silently truncated
     * to something a shorter key would also produce. */
    uint8_t huge[sizeof(s_ap_secret) + 1];
    memset(huge, 0xAB, sizeof(huge));
    wifi_manager_set_ap_secret(huge, sizeof(huge));
    TEST_ASSERT_EQUAL_INT(-1, wifi_manager_get_ap_password(pw, sizeof(pw)));

    /* And a buffer too small for a maximum-length override is refused. */
    uint8_t secret[64];
    fake_secret(secret, 2);
    wifi_manager_set_ap_secret(secret, sizeof(secret));
    char small[WIFI_AP_PASSWORD_FIELD - 1];
    TEST_ASSERT_EQUAL_INT(-1, wifi_manager_get_ap_password(small, sizeof(small)));
}

void test_station_event_transitions_disconnected_and_got_ip(void) {
    strcpy(g_nvs_ssid, "SavedNet");
    strcpy(g_nvs_pass, "savedpass");
    g_wait_bits_result = BIT0;
    TEST_ASSERT_EQUAL_INT(0, wifi_manager_init(0x1234));

    ip_event_got_ip_t got_ip = {0};
    got_ip.ip_info.ip.addr = ESP_IP4TOADDR(10, 0, 0, 42);
    g_sta_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip);

    wifi_status_t status = {0};
    wifi_manager_get_status(&status);
    TEST_ASSERT_EQUAL_STRING("10.0.0.42", status.ip_addr);

    g_ws_running = true;
    wifi_event_sta_disconnected_t disc = {.reason = 7};
    g_sta_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disc);

    TEST_ASSERT_EQUAL_INT(1, g_ws_stop_calls);
    TEST_ASSERT_TRUE((g_set_bits_accum & BIT1) != 0);
    TEST_ASSERT_TRUE(g_wifi_connect_calls >= 1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_nvs_roundtrip_and_clear);
    RUN_TEST(test_nvs_get_creds_treats_missing_password_as_open_network);
    RUN_TEST(test_init_prefers_station_with_saved_creds);
    RUN_TEST(test_init_falls_back_to_ap_when_station_fails);
    RUN_TEST(test_ap_mode_refuses_to_start_without_a_password);
    RUN_TEST(test_ap_password_is_stable_across_reinit);
    RUN_TEST(test_get_ap_password_fails_closed_with_no_secret);
    RUN_TEST(test_station_event_transitions_disconnected_and_got_ip);
    return UNITY_END();
}
