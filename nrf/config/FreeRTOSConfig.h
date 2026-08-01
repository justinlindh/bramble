// FreeRTOS configuration for the Bramble nRF52840 target (Cortex-M4F, 64MHz).
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern uint32_t SystemCoreClock;

#define configUSE_PREEMPTION 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
// Deliberately off, a considered decision rather than an unset placeholder:
// tickless idle is unverified on this port, and this project's own
// mesh loop assumes a steady 1ms tick. NimBLE's host stack schedules its
// own timers (connection/GAP/SM timeouts) as FreeRTOS software timers,
// tick-driven by design (npl_freertos_callout_reset's xTimerChangePeriod,
// porting/npl/freertos/src/npl_os_freertos.c in the fetched nimble source),
// and this project has not checked that those correctly resync after a
// tickless sleep. Radio/link-layer timing is unaffected either way: NimBLE's
// controller schedules radio events off os_cputime (ble_ll_tmr_get/start in
// nimble/controller/include/controller/ble_ll_tmr.h, called from
// nimble/controller/src/ble_ll_sched.c), which on this build is backed by
// hardware RTC0, not SysTick (MYNEWT_VAL(OS_CPUTIME_TIMER_NUM) is 5 in
// nrf/config/nimble/syscfg/syscfg.h, and case 5 in
// porting/nimble/src/hal_timer.c maps to NRF_RTC0/RTC0_IRQn); tickless idle
// only reprograms SysTick, so it could only ever affect the host stack's own
// timers and this project's mesh loop, never radio timing. The CPU still
// sleeps between ticks today via WFE (vApplicationIdleHook, main_nrf.c);
// tickless would extend that to sleeping across multiple ticks. Revisit
// once the mesh loop becomes event-driven instead of tick-polled, and
// verify NimBLE's host timers resync correctly before flipping this on.
#define configUSE_TICKLESS_IDLE 0
#define configCPU_CLOCK_HZ (SystemCoreClock)
#define configTICK_RATE_HZ ((TickType_t)1000)
/* 9, not 8: NimBLE's port creates its link-layer task at
 * configMAX_PRIORITIES-1, which must sit ABOVE the FreeRTOS timer task
 * (7) and the mesh task (5) to keep radio events on time. */
#define configMAX_PRIORITIES 9
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
// This is the ONLY heap on the target: shim/malloc_freertos.c routes newlib's
// malloc/calloc here too, so it must cover both the heap_caps_* callers and
// the plain-calloc ones. The large tenants are mesh_task_start's DM session
// table (44,160 bytes) and delivery event ring (28,692 bytes), which have no
// PSRAM to fall back to here, plus task stacks, the message store, and
// mbedtls (including the P-256 keypair each BLE pairing builds).
//
// The size is what is left of the 256KB after static data and the 8KB
// interrupt stack, less a small unallocated margin. Raising it further means
// taking RAM the linker has already committed elsewhere, so grow the budget
// gate and this together or not at all.
/* 148KB: was 152KB until the NimBLE msys pool went 12 -> 24 blocks (3.9KB
 * of statics) after a real phone session exhausted the smaller pool; the
 * only place on a 256KB chip that margin can come from is here. Still 4KB
 * above the 144KB floor the size gate enforces. */
#define configTOTAL_HEAP_SIZE ((size_t)(148 * 1024))
#define configUSE_MALLOC_FAILED_HOOK 1
#define configCHECK_FOR_STACK_OVERFLOW 2

// vApplicationIdleHook sleeps the CPU (WFE) each idle pass; see main_nrf.c.
#define configUSE_IDLE_HOOK 1
#define configUSE_TICK_HOOK 0

#define configUSE_TIMERS 1
#define configTIMER_TASK_PRIORITY (configMAX_PRIORITIES - 2)
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
#define INCLUDE_pcTaskGetTaskName 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_xTaskGetHandle 1
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
