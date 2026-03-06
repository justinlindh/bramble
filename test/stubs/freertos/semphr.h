#ifndef FREERTOS_SEMPHR_H_STUB
#define FREERTOS_SEMPHR_H_STUB

#include "FreeRTOS.h"

typedef void *SemaphoreHandle_t;

#define xSemaphoreCreateMutex()           ((SemaphoreHandle_t)1)
#define xSemaphoreTake(s, t)              (1)
#define xSemaphoreGive(s)                 (1)
#define vSemaphoreDelete(s)               ((void)(s))

#endif
