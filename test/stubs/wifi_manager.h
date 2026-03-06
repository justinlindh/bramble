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

typedef struct {
    bramble_wifi_mode_t mode;
    char ip_addr[16];
    char ssid[33];
    int8_t rssi;
} wifi_status_t;

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

#endif
