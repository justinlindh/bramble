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
/* Everything on this chip is internal 8-bit-addressable SRAM, so these
 * select the same heap as MALLOC_CAP_DEFAULT; they exist because the
 * diagnostics RPC asks for heap figures by capability. */
#define MALLOC_CAP_INTERNAL (1 << 2)
#define MALLOC_CAP_8BIT (1 << 3)
#define MALLOC_CAP_DMA (1 << 4)

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

static inline size_t heap_caps_get_free_size(int caps) {
    (void)caps;
    return xPortGetFreeHeapSize();
}

static inline size_t heap_caps_get_minimum_free_size(int caps) {
    (void)caps;
    return xPortGetMinimumEverFreeHeapSize();
}

/* IDF dumps a per-block heap map to the console; heap_4 exposes no
 * equivalent walk, and the summary figures above carry the diagnostic
 * value, so this is deliberately a no-op rather than a fake dump. */
static inline void heap_caps_dump(int caps) { (void)caps; }

static inline size_t heap_caps_get_largest_free_block(int caps) {
    (void)caps;
    /* heap_4 tracks this properly, so report the real number rather than an
     * approximation: fragmentation is exactly what this figure is for. */
    HeapStats_t stats;
    vPortGetHeapStats(&stats);
    return stats.xSizeOfLargestFreeBlockInBytes;
}

#ifdef __cplusplus
}
#endif
