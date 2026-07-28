// FreeRTOS configuration for the Bramble nRF52840 target (Cortex-M4F, 64MHz).
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern uint32_t SystemCoreClock;

#define configUSE_PREEMPTION 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE 0 // P3 power work turns this on
#define configCPU_CLOCK_HZ (SystemCoreClock)
#define configTICK_RATE_HZ ((TickType_t)1000)
#define configMAX_PRIORITIES 8
#define configMINIMAL_STACK_SIZE 128
#define configMAX_TASK_NAME_LEN 12
#define configUSE_16_BIT_TICKS 0
#define configIDLE_SHOULD_YIELD 1

#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configQUEUE_REGISTRY_SIZE 0
#define configUSE_QUEUE_SETS 0
#define configUSE_TASK_NOTIFICATIONS 1

#define configSUPPORT_STATIC_ALLOCATION 1
#define configKERNEL_PROVIDED_STATIC_MEMORY 1
#define configSUPPORT_DYNAMIC_ALLOCATION 1
// 96K, not 48K: mesh_task_start heap-allocates the DM session table
// (44,160 bytes) and the delivery event ring (28,692 bytes); both fell back
// to a too-small heap and silently disabled the mesh before this was sized
// for them.
#define configTOTAL_HEAP_SIZE ((size_t)(96 * 1024))
#define configUSE_MALLOC_FAILED_HOOK 1
#define configCHECK_FOR_STACK_OVERFLOW 2

#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0

#define configUSE_TIMERS 1
#define configTIMER_TASK_PRIORITY (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH 16
// The esp_timer shim runs its callbacks on the timer task, so it gets a
// real stack (in words).
#define configTIMER_TASK_STACK_DEPTH 512

#define INCLUDE_vTaskPrioritySet 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskDelayUntil 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTimerPendFunctionCall 1
#define INCLUDE_xSemaphoreGetMutexHolder 1

// nRF52840 NVIC has 3 priority bits (values 0..7, shifted into bits 7:5).
// Kernel runs at the lowest priority; ISRs using FreeRTOS APIs must be
// numerically >= 2 (lower urgency than 2 is not allowed to call the API).
#define configPRIO_BITS 3
#define configKERNEL_INTERRUPT_PRIORITY (7 << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (2 << (8 - configPRIO_BITS))

// Map the port handlers onto the MDK startup vector names.
#define vPortSVCHandler SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

extern void bramble_assert_failed(const char* file, int line);
#define configASSERT(x)                                                                            \
    do {                                                                                           \
        if ((x) == 0) {                                                                            \
            bramble_assert_failed(__FILE__, __LINE__);                                             \
        }                                                                                          \
    } while (0)

#endif // FREERTOS_CONFIG_H
