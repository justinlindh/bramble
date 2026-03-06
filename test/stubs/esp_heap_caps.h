#ifndef TEST_STUB_ESP_HEAP_CAPS_H
#define TEST_STUB_ESP_HEAP_CAPS_H

#include <stdlib.h>

#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_DMA 0

static inline void* heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

static inline size_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return 1024 * 1024;
}

static inline size_t heap_caps_get_minimum_free_size(uint32_t caps) {
    (void)caps;
    return 512 * 1024;
}

static inline size_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return 256 * 1024;
}

static inline void heap_caps_dump(uint32_t caps) { (void)caps; }

#endif
