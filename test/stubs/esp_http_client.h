#ifndef ESP_HTTP_CLIENT_H_STUB
#define ESP_HTTP_CLIENT_H_STUB

#include "esp_stubs.h"
#include <stdbool.h>

typedef struct {
    const char* url;
    int timeout_ms;
    bool skip_cert_common_name_check;
    void* crt_bundle_attach;
} esp_http_client_config_t;

typedef void* esp_http_client_handle_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char* buffer, int len);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
void esp_http_client_close(esp_http_client_handle_t client);
void esp_http_client_cleanup(esp_http_client_handle_t client);

#endif
