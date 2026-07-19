#ifndef WIFI_MANAGER_H_STUB
#define WIFI_MANAGER_H_STUB

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    BRAMBLE_WIFI_OFF,
    BRAMBLE_WIFI_STATION,
    BRAMBLE_WIFI_AP,
} bramble_wifi_mode_t;

/* Mirrors components/wifi/include/wifi_manager.h. */
#define WIFI_AP_PASSWORD_FIELD 64

typedef struct {
    bramble_wifi_mode_t mode;
    char ip_addr[16];
    char ssid[33];
    int8_t rssi;
    char ap_password[WIFI_AP_PASSWORD_FIELD];
} wifi_status_t;

/* When testing the real wifi_manager.c, skip these stub implementations
   to avoid redefinition errors. Define WIFI_MANAGER_TEST before including. */
#ifndef WIFI_MANAGER_TEST
static inline int wifi_manager_init(uint32_t node_addr) {
    (void)node_addr;
    return 0;
}
static inline void wifi_manager_get_status(wifi_status_t* status) {
    if (status)
        memset(status, 0, sizeof(*status));
}
static inline const char* wifi_manager_get_ip(void) { return ""; }
static inline int wifi_manager_nvs_get_creds(char* ssid, size_t ssid_len, char* password,
                                             size_t pass_len) {
    (void)ssid;
    (void)ssid_len;
    (void)password;
    (void)pass_len;
    return -1;
}
static inline int wifi_manager_nvs_set_creds(const char* ssid, const char* password) {
    (void)ssid;
    (void)password;
    return -1;
}
static inline int wifi_manager_nvs_clear_creds(void) { return -1; }
static inline void wifi_manager_set_ap_secret(const uint8_t* secret, size_t secret_len) {
    (void)secret;
    (void)secret_len;
}
static inline int wifi_manager_get_ap_password(char* out, size_t out_len) {
    if (out && out_len > 0)
        out[0] = '\0';
    return -1;
}
#else
/* Declarations only, implementations come from the real wifi_manager.c */
int wifi_manager_init(uint32_t node_addr);
void wifi_manager_get_status(wifi_status_t* status);
const char* wifi_manager_get_ip(void);
int wifi_manager_nvs_get_creds(char* ssid, size_t ssid_len, char* password, size_t pass_len);
int wifi_manager_nvs_set_creds(const char* ssid, const char* password);
int wifi_manager_nvs_clear_creds(void);
void wifi_manager_set_ap_secret(const uint8_t* secret, size_t secret_len);
int wifi_manager_get_ap_password(char* out, size_t out_len);
#endif

#endif
