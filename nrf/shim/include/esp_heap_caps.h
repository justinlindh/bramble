// esp_heap_caps shim for the nRF52840 target: everything maps onto the
// FreeRTOS heap. Capability flags are accepted and ignored; there is no
// PSRAM on this chip, so MALLOC_CAP_SPIRAM requests are expected to fail and
// callers fall back to MALLOC_CAP_DEFAULT (msg_store.c does exactly this).
#pragma once

#include <stddef.h>
#include <string.h>

#include <FreeRTOS.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_DEFAULT 0
#define MALLOC_CAP_SPIRAM (1 << 1)

static inline void* heap_caps_malloc(size_t size, int caps) {
    if (caps & MALLOC_CAP_SPIRAM) {
        return NULL; // no PSRAM on nRF52840; callers fall back
    }
    return pvPortMalloc(size);
}

static inline void* heap_caps_calloc(size_t n, size_t size, int caps) {
    if (caps & MALLOC_CAP_SPIRAM) {
        return NULL;
    }
    size_t total = n * size;
    if (size != 0 && total / size != n) {
        return NULL; // multiplication overflow
    }
    void* p = pvPortMalloc(total);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

static inline void heap_caps_free(void* ptr) { vPortFree(ptr); }

#ifdef __cplusplus
}
#endif
