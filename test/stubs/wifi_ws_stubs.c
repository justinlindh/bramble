/*
 * Minimal stubs for WiFi and WebSocket server symbols referenced by rpc_methods.c
 * but not provided by esp_stubs.c or rpc_methods_test_stubs.c.
 * Include this stub file in test targets that do not supply their own definitions.
 */
#include <stdint.h>
#include "esp_wifi.h"

const char *ws_server_get_token(void) { return ""; }
esp_err_t esp_wifi_get_mac(int ifx, uint8_t mac[6]) { (void)ifx; (void)mac; return 1; /* ESP_FAIL */ }
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *list) { (void)list; return 1; /* ESP_FAIL */ }
