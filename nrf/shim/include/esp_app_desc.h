// esp_app_desc shim for the nRF52840 target: the two fields the RPC surface
// reads, same shape as test/stubs (rpc_methods.c compiles against both).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* version;
    uint32_t secure_version;
} esp_app_desc_t;

const esp_app_desc_t* esp_app_get_description(void);

#ifdef __cplusplus
}
#endif
