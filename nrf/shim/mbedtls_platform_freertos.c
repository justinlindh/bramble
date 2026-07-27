// Routes mbedtls heap allocation onto the FreeRTOS heap (MBEDTLS_PLATFORM_MEMORY)
// so the target has a single accounted heap; called once from main before any
// crypto use.
#include <string.h>

#include <FreeRTOS.h>

#include <mbedtls/platform.h>

static void* freertos_calloc(size_t n, size_t size) {
    size_t total = n * size;
    if (size != 0 && total / size != n) {
        return NULL;
    }
    void* p = pvPortMalloc(total);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

static void freertos_free(void* p) { vPortFree(p); }

void bramble_mbedtls_platform_init(void) {
    mbedtls_platform_set_calloc_free(freertos_calloc, freertos_free);
}
