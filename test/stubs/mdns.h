#ifndef MDNS_H_STUB
#define MDNS_H_STUB

#include "esp_stubs.h"

/* Host stub: mDNS only exists on-device. rpc_methods.c updates the TXT
 * record best-effort on rename; on host that is a no-op. */
static inline esp_err_t mdns_service_txt_item_set(const char* service, const char* proto,
                                                  const char* key, const char* value) {
    (void)service;
    (void)proto;
    (void)key;
    (void)value;
    return ESP_OK;
}

#endif
