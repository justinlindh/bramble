#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static wifi_status_t s_status = {
    .mode    = WIFI_MODE_OFF,
    .ip_addr = "",
    .ssid    = "",
    .rssi    = 0,
};

/* ── Event handlers ──────────────────────────────────────────────────── */

static void sta_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Station disconnected — not retrying during initial connect");
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_status.ip_addr, sizeof(s_status.ip_addr),
                 IPSTR, IP2STR(&event->ip_info.ip));
        s_status.mode = WIFI_MODE_STATION;
        ESP_LOGI(TAG, "Got IP: %s", s_status.ip_addr);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── Station mode ────────────────────────────────────────────────────── */

static int try_station_mode(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_event_group = xEventGroupCreate();

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &sta_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &sta_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid,
            CONFIG_BRAMBLE_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password,
            CONFIG_BRAMBLE_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for station connect (timeout %ds)...",
             CONFIG_BRAMBLE_WIFI_STA_TIMEOUT_S);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(CONFIG_BRAMBLE_WIFI_STA_TIMEOUT_S * 1000));

    /* Unregister handlers regardless of outcome */
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;

    if (bits & WIFI_CONNECTED_BIT) {
        strncpy(s_status.ssid, CONFIG_BRAMBLE_WIFI_SSID, sizeof(s_status.ssid) - 1);
        ESP_LOGI(TAG, "Station mode connected: SSID=%s IP=%s",
                 s_status.ssid, s_status.ip_addr);
        return 0;
    }

    ESP_LOGW(TAG, "Station connect failed or timed out");
    esp_wifi_stop();
    esp_wifi_deinit();
    return -1;
}

/* ── AP mode ─────────────────────────────────────────────────────────── */

static int start_ap_mode(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid,
            CONFIG_BRAMBLE_WIFI_AP_SSID,
            sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char *)ap_cfg.ap.password,
            CONFIG_BRAMBLE_WIFI_AP_PASSWORD,
            sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len     = (uint8_t)strlen(CONFIG_BRAMBLE_WIFI_AP_SSID);
    ap_cfg.ap.channel      = 1;
    ap_cfg.ap.authmode     = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* AP netif always uses 192.168.4.1 */
    strncpy(s_status.ip_addr, "192.168.4.1", sizeof(s_status.ip_addr) - 1);
    strncpy(s_status.ssid, CONFIG_BRAMBLE_WIFI_AP_SSID, sizeof(s_status.ssid) - 1);
    s_status.mode = WIFI_MODE_AP;

    ESP_LOGI(TAG, "AP mode: %s (%s)", CONFIG_BRAMBLE_WIFI_AP_SSID, s_status.ip_addr);
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int wifi_manager_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if defined(CONFIG_BRAMBLE_WIFI_SSID) && (CONFIG_BRAMBLE_WIFI_SSID[0] != '\0')
    ESP_LOGI(TAG, "SSID configured — trying station mode: %s", CONFIG_BRAMBLE_WIFI_SSID);
    if (try_station_mode() == 0) {
        return 0;
    }
    ESP_LOGW(TAG, "Station failed — falling back to AP mode");
#else
    ESP_LOGI(TAG, "No SSID configured — starting AP mode directly");
#endif

    return start_ap_mode();
}

void wifi_manager_get_status(wifi_status_t *status)
{
    if (status) {
        *status = s_status;
    }
}

const char *wifi_manager_get_ip(void)
{
    return s_status.ip_addr;
}
