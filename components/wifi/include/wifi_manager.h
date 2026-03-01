#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BRAMBLE_WIFI_OFF,
    BRAMBLE_WIFI_STATION,
    BRAMBLE_WIFI_AP,
} bramble_wifi_mode_t;

typedef struct {
    bramble_wifi_mode_t mode;
    char ip_addr[16];
    char ssid[33];
    int8_t rssi;
} wifi_status_t;

/* Initialize WiFi. Checks NVS creds first, then Kconfig, then AP fallback. */
int wifi_manager_init(uint32_t node_addr);

/* Get current WiFi status */
void wifi_manager_get_status(wifi_status_t *status);

/* Get current IP as string. Returns empty string if not connected. */
const char *wifi_manager_get_ip(void);

/* NVS credential management — survives reflash */
int wifi_manager_nvs_get_creds(char *ssid, size_t ssid_len,
                                char *password, size_t pass_len);
int wifi_manager_nvs_set_creds(const char *ssid, const char *password);
int wifi_manager_nvs_clear_creds(void);
