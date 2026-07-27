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
#define MALLOC_CAP_INTERNAL (1 << 0)
#define MALLOC_CAP_SPIRAM (1 << 1)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)

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
    void* p = pvPortMalloc(n * size);
    if (p != NULL) {
        memset(p, 0, n * size);
    }
    return p;
}

static inline void heap_caps_free(void* ptr) { vPortFree(ptr); }

static inline size_t heap_caps_get_free_size(int caps) {
    (void)caps;
    return xPortGetFreeHeapSize();
}

#ifdef __cplusplus
}
#endif
