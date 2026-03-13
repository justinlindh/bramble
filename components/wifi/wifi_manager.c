#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char* TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

/* NVS namespace and key names are defined in nvs_keys.h */
#define NVS_NAMESPACE       NVS_NS_WIFI
#define NVS_KEY_SSID        NVS_KEY_WIFI_SSID
#define NVS_KEY_PASSWORD    NVS_KEY_WIFI_PASSWORD

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_event_handler_instance_t s_sta_any_id;
static esp_event_handler_instance_t s_sta_got_ip;
static bool s_sta_handlers_registered = false;

static wifi_status_t s_status = {
    .mode = BRAMBLE_WIFI_OFF,
    .ip_addr = "",
    .ssid = "",
    .rssi = 0,
};

/* ── NVS helpers ─────────────────────────────────────────────────────── */

int wifi_manager_nvs_get_creds(char* ssid, size_t ssid_len, char* password, size_t pass_len) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK)
        return -1;

    err = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK || ssid[0] == '\0') {
        nvs_close(nvs);
        return -1;
    }

    err = nvs_get_str(nvs, NVS_KEY_PASSWORD, password, &pass_len);
    if (err != ESP_OK) {
        password[0] = '\0'; /* Open network — no password */
    }

    nvs_close(nvs);
    return 0;
}

int wifi_manager_nvs_set_creds(const char* ssid, const char* password) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
        return -1;

    nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    nvs_set_str(nvs, NVS_KEY_PASSWORD, password ? password : "");
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "WiFi credentials saved to NVS (SSID: %s)", ssid);
    return 0;
}

int wifi_manager_nvs_clear_creds(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
        return -1;

    nvs_erase_key(nvs, NVS_KEY_SSID);
    nvs_erase_key(nvs, NVS_KEY_PASSWORD);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "WiFi credentials cleared from NVS");
    return 0;
}

/* ── Event handlers ──────────────────────────────────────────────────── */

static void sta_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                              void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t* disc =
            (const wifi_event_sta_disconnected_t*)event_data;
        if (disc) {
            ESP_LOGW(TAG, "Station disconnected (reason=%d) — reconnecting", disc->reason);
        } else {
            ESP_LOGW(TAG, "Station disconnected — reconnecting");
        }

        /* Invalidate stale IP immediately so status reflects reality. */
        s_status.ip_addr[0] = '\0';

        /* Stop WebSocket server during disconnection to free resources.
         * It will restart automatically when we get an IP on reconnection. */
        extern void ws_server_stop(void);
        extern bool ws_server_is_running(void);
        if (ws_server_is_running()) {
            ESP_LOGI(TAG, "Stopping WebSocket server during disconnection");
            ws_server_stop();
        }

        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

        /* Keep station alive as first-class transport: always retry. */
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        snprintf(s_status.ip_addr, sizeof(s_status.ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        s_status.mode = BRAMBLE_WIFI_STATION;
        ESP_LOGI(TAG, "Got IP: %s", s_status.ip_addr);

        /* Start WebSocket server now that we have an IP.
         * ws_server_start() is idempotent, safe to call multiple times.
         * This ensures the server starts on:
         *   - Initial WiFi connection at boot
         *   - Reconnection after temporary disconnect
         *   - AP → Station mode transition */
        extern int ws_server_start(void);
        ws_server_start();

        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

/* ── Station mode ────────────────────────────────────────────────────── */

static int try_station_mode(const char* ssid, const char* password) {
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }

    if (!s_sta_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL, &s_sta_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, &s_sta_got_ip));
        s_sta_handlers_registered = true;
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char*)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char*)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for station connect (timeout %ds)...",
             CONFIG_BRAMBLE_WIFI_STA_TIMEOUT_S);

    EventBits_t bits =
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE,
                            pdFALSE, pdMS_TO_TICKS(CONFIG_BRAMBLE_WIFI_STA_TIMEOUT_S * 1000));

    if (bits & WIFI_CONNECTED_BIT) {
        strncpy(s_status.ssid, ssid, sizeof(s_status.ssid) - 1);
        ESP_LOGI(TAG, "Station mode connected: SSID=%s IP=%s", s_status.ssid, s_status.ip_addr);
        return 0;
    }

    ESP_LOGW(TAG, "Station connect failed or timed out");

    if (s_sta_handlers_registered) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_sta_got_ip);
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_sta_any_id);
        s_sta_handlers_registered = false;
    }
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    if (sta_netif) {
        esp_netif_destroy(sta_netif);
    }
    return -1;
}

/* ── AP mode ─────────────────────────────────────────────────────────── */

static int start_ap_mode(uint32_t node_addr) {
    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Build unique AP SSID: Bramble-XXXX (last 4 hex of node address) */
    char ap_ssid[33];
    snprintf(ap_ssid, sizeof(ap_ssid), "Bramble-%04" PRIX32, node_addr & 0xFFFF);

    wifi_config_t ap_cfg = {0};
    strncpy((char*)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char*)ap_cfg.ap.password, CONFIG_BRAMBLE_WIFI_AP_PASSWORD,
            sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    if (ap_netif) {
        esp_netif_ip_info_t ip_info = {
            .ip.addr = ESP_IP4TOADDR(192, 168, 4, 1),
            .gw.addr = ESP_IP4TOADDR(192, 168, 4, 1),
            .netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0),
        };

        /* Ensure deterministic AP DHCP server state before start. */
        esp_err_t dhcps_err = esp_netif_dhcps_stop(ap_netif);
        if (dhcps_err != ESP_OK && dhcps_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_ERROR_CHECK(dhcps_err);
        }

        ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));

        dhcps_err = esp_netif_dhcps_start(ap_netif);
        if (dhcps_err != ESP_OK && dhcps_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_ERROR_CHECK(dhcps_err);
        }
    }

    esp_log_level_set("wifi", ESP_LOG_ERROR);
    ESP_ERROR_CHECK(esp_wifi_start());

    strncpy(s_status.ip_addr, "192.168.4.1", sizeof(s_status.ip_addr) - 1);
    strncpy(s_status.ssid, ap_ssid, sizeof(s_status.ssid) - 1);
    s_status.mode = BRAMBLE_WIFI_AP;

    ESP_LOGI(TAG, "AP mode: %s (%s)", ap_ssid, s_status.ip_addr);

    /* Start WebSocket server on AP mode so webapp can connect */
    extern int ws_server_start(void);
    ws_server_start();
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int wifi_manager_init(uint32_t node_addr) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Check NVS for saved credentials first */
    char nvs_ssid[33] = {0};
    char nvs_pass[65] = {0};
    if (wifi_manager_nvs_get_creds(nvs_ssid, sizeof(nvs_ssid), nvs_pass, sizeof(nvs_pass)) == 0) {
        ESP_LOGI(TAG, "Found NVS WiFi credentials — trying station: %s", nvs_ssid);
        if (try_station_mode(nvs_ssid, nvs_pass) == 0) {
            return 0;
        }
        ESP_LOGW(TAG, "NVS station failed — falling back to AP");
    }

    /* Fall back to Kconfig defaults */
    if (strlen(CONFIG_BRAMBLE_WIFI_SSID) > 0) {
        ESP_LOGI(TAG, "Trying Kconfig SSID: %s", CONFIG_BRAMBLE_WIFI_SSID);
        if (try_station_mode(CONFIG_BRAMBLE_WIFI_SSID, CONFIG_BRAMBLE_WIFI_PASSWORD) == 0) {
            return 0;
        }
        ESP_LOGW(TAG, "Kconfig station failed — falling back to AP");
    } else {
        ESP_LOGI(TAG, "No WiFi credentials — starting AP mode");
    }

    return start_ap_mode(node_addr);
}

void wifi_manager_get_status(wifi_status_t* status) {
    if (status) {
        *status = s_status;
    }
}

const char* wifi_manager_get_ip(void) { return s_status.ip_addr; }
