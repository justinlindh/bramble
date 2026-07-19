#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wifi_ap_password.h"

/* Buffer sized for either a derived password or a maximum-length explicit
 * WPA2-PSK override, plus the NUL. */
#define WIFI_AP_PASSWORD_FIELD (WIFI_AP_PASSWORD_MAX + 1)

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
    /* The live SoftAP password, populated only in BRAMBLE_WIFI_AP mode and
     * empty otherwise. This is the surface the on-device UIs read so a user
     * holding the device can join its AP. */
    char ap_password[WIFI_AP_PASSWORD_FIELD];
} wifi_status_t;

/*
 * Provide the secret identity material the per-device AP password derives
 * from. Must be called BEFORE wifi_manager_init on any node that may fall
 * back to AP mode: with no secret and no explicit Kconfig override, AP mode
 * fails closed rather than starting on a guessable PSK. The buffer is copied.
 */
void wifi_manager_set_ap_secret(const uint8_t* secret, size_t secret_len);

/*
 * Resolve the AP password: the CONFIG_BRAMBLE_WIFI_AP_PASSWORD override when
 * it is set and within the WPA2-PSK length bounds, otherwise the value
 * derived from the provisioned secret. out_len must be at least
 * WIFI_AP_PASSWORD_FIELD. Returns 0 on success, -1 if no password can be
 * produced (leaving out empty).
 */
int wifi_manager_get_ap_password(char* out, size_t out_len);

/* Initialize WiFi. Checks NVS creds first, then Kconfig, then AP fallback. */
int wifi_manager_init(uint32_t node_addr);

/* Get current WiFi status */
void wifi_manager_get_status(wifi_status_t* status);

/* Get current IP as string. Returns empty string if not connected. */
const char* wifi_manager_get_ip(void);

/* NVS credential management — survives reflash */
int wifi_manager_nvs_get_creds(char* ssid, size_t ssid_len, char* password, size_t pass_len);
int wifi_manager_nvs_set_creds(const char* ssid, const char* password);
int wifi_manager_nvs_clear_creds(void);
