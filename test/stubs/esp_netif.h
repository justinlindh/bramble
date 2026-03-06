#ifndef ESP_NETIF_H_STUB
#define ESP_NETIF_H_STUB

#include "esp_stubs.h"
#include <stdint.h>

typedef struct esp_netif_t {
    int dummy;
} esp_netif_t;

typedef struct {
    struct {
        uint32_t addr;
    } ip;
    struct {
        uint32_t addr;
    } gw;
    struct {
        uint32_t addr;
    } netmask;
} esp_netif_ip_info_t;

#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 0x5001
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 0x5002

#define ESP_IP4TOADDR(a, b, c, d) ((uint32_t)(((a) << 24) | ((b) << 16) | ((c) << 8) | (d)))
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(ipaddr_ptr)                                                                        \
    (int)((((ipaddr_ptr)->addr) >> 24) & 0xFF), (int)((((ipaddr_ptr)->addr) >> 16) & 0xFF),     \
        (int)((((ipaddr_ptr)->addr) >> 8) & 0xFF), (int)(((ipaddr_ptr)->addr) & 0xFF)

typedef struct {
    esp_netif_ip_info_t ip_info;
} ip_event_got_ip_t;

esp_err_t esp_netif_init(void);
esp_netif_t* esp_netif_create_default_wifi_sta(void);
esp_netif_t* esp_netif_create_default_wifi_ap(void);
void esp_netif_destroy(esp_netif_t* netif);
esp_err_t esp_netif_set_ip_info(esp_netif_t* netif, const esp_netif_ip_info_t* ip_info);
esp_err_t esp_netif_dhcps_stop(esp_netif_t* netif);
esp_err_t esp_netif_dhcps_start(esp_netif_t* netif);

#endif
