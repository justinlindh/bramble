#pragma once

#include <stddef.h>

// Bring up the portable Bramble stack (see app_init.c). Call from a task
// with at least 2KB of stack: identity generation runs mbedtls ECP.
void app_init_stack(void);

// Free-heap probe, implemented by main (FreeRTOS xPortGetFreeHeapSize).
size_t heap_free_probe(void);
