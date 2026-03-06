#ifndef ESP_WIFI_H_STUB
#define ESP_WIFI_H_STUB
#include "esp_stubs.h"
#include <string.h>

typedef struct {
    int dummy;
} wifi_config_t;
typedef struct {
    int dummy;
} wifi_ap_record_t;

#define ESP_IF_WIFI_STA 0
#define WIFI_IF_AP 0
#define WIFI_IF_STA 1

typedef struct {
    int num;
} wifi_sta_list_t;

static inline esp_err_t esp_wifi_get_mac(int ifx, uint8_t mac[6]) {
    (void)ifx;
    memset(mac, 0, 6);
    return ESP_OK;
}

static inline esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t* list) {
    if (list)
        memset(list, 0, sizeof(*list));
    return ESP_OK;
}

#endif
