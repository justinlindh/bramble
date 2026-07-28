#include "esp_system.h"

#include <FreeRTOS.h>

#include <nrfx.h>

void esp_restart(void) { NVIC_SystemReset(); }

uint32_t esp_get_free_heap_size(void) { return (uint32_t)xPortGetFreeHeapSize(); }
