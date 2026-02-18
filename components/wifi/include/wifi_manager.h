#pragma once
#include <stdbool.h>

typedef enum {
    WIFI_MODE_OFF,
    WIFI_MODE_STATION,
    WIFI_MODE_AP,
} bramble_wifi_mode_t;

typedef struct {
    bramble_wifi_mode_t mode;
    char ip_addr[16];
    char ssid[33];
    int8_t rssi;
} wifi_status_t;

/* Initialize WiFi. Tries station mode first if SSID configured, falls back to AP. */
int wifi_manager_init(void);

/* Get current WiFi status */
void wifi_manager_get_status(wifi_status_t *status);

/* Get current IP as string. Returns empty string if not connected. */
const char *wifi_manager_get_ip(void);
