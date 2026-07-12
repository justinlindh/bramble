#ifndef FREERTOS_SEMPHR_H_STUB
#define FREERTOS_SEMPHR_H_STUB

#include "FreeRTOS.h"

typedef void* SemaphoreHandle_t;

/* Inline functions (not bare-value macros) so that ignoring the return in a
 * statement context does not trip -Wunused-value on the host build. */
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (SemaphoreHandle_t)1; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned long t) {
    (void)s;
    (void)t;
    return 1;
}
static inline int xSemaphoreGive(SemaphoreHandle_t s) {
    (void)s;
    return 1;
}
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }

#endif
