// esp_system shim for the nRF52840 target.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp_restart(void);

// Free bytes in the FreeRTOS heap (the target's one accounted heap).
uint32_t esp_get_free_heap_size(void);

#ifdef __cplusplus
}
#endif
