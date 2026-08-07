#ifndef ESP_WIFI_H_STUB
#define ESP_WIFI_H_STUB

#include "esp_stubs.h"
#include <stdint.h>
#include <string.h>

typedef int wifi_mode_t;
typedef int wifi_interface_t;
typedef int wifi_auth_mode_t;

enum {
    WIFI_MODE_STA = 1,
    WIFI_MODE_AP = 2,
};

enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WPA2_PSK = 3,
};

#define WIFI_IF_AP 0
#define WIFI_IF_STA 1

typedef struct {
    int dummy;
} wifi_init_config_t;

#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    struct {
        wifi_auth_mode_t authmode;
    } threshold;
} wifi_sta_config_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    uint8_t ssid_len;
    uint8_t channel;
    wifi_auth_mode_t authmode;
    uint8_t max_connection;
} wifi_ap_config_t;

typedef struct {
    wifi_sta_config_t sta;
    wifi_ap_config_t ap;
} wifi_config_t;

typedef struct {
    int reason;
} wifi_event_sta_disconnected_t;

#define WIFI_EVENT_STA_START 100
#define WIFI_EVENT_STA_DISCONNECTED 101

typedef struct {
    int num;
} wifi_sta_list_t;

typedef enum {
    WIFI_PS_NONE = 0,
    WIFI_PS_MIN_MODEM = 1,
    WIFI_PS_MAX_MODEM = 2,
} wifi_ps_type_t;

esp_err_t esp_wifi_init(const wifi_init_config_t* config);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t* conf);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_get_mac(int ifx, uint8_t mac[6]);
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t* list);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);
esp_err_t esp_wifi_set_max_tx_power(int8_t power);

#endif
