/**
 * @file lv_malloc_psram.c
 *
 * T-Deck Plus LVGL allocator override.
 *
 * Routes LVGL core allocations to PSRAM when CONFIG_LV_USE_CUSTOM_MALLOC is enabled
 * to reduce pressure on scarce internal RAM during heavy Settings screen creation.
 */

#include "sdkconfig.h"

#if defined(CONFIG_BRAMBLE_UI_GRAPHICAL) && defined(CONFIG_LV_USE_CUSTOM_MALLOC)

#include <stdlib.h>
#include <stddef.h>
#include "esp_heap_caps.h"
#include "lvgl.h"

/* These override lv_malloc_core etc. from lv_mem_core_clib.c.
 * We achieve this by registering this file before the managed component
 * in the linker order, combined with LV_STDLIB_CUSTOM in lv_conf.h.
 *
 * With LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM, LVGL calls:
 *   lv_malloc_core, lv_realloc_core, lv_free_core, lv_mem_init, lv_mem_deinit
 */

void lv_mem_init(void) { /* nothing: heap_caps is always ready */
}

void lv_mem_deinit(void) { /* nothing */
}

void* lv_malloc_core(size_t size) {
    /* Allocate strictly from PSRAM to avoid internal-heap corruption path. */
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void* lv_realloc_core(void* p, size_t new_size) {
    /* heap_caps_realloc preserves the original region caps */
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
}

void lv_free_core(void* p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) {
    if (!mon_p)
        return;
    mon_p->total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    mon_p->free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mon_p->max_used = mon_p->total_size - mon_p->free_size;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

#endif /* CONFIG_BRAMBLE_UI_GRAPHICAL && CONFIG_LV_USE_CUSTOM_MALLOC */
