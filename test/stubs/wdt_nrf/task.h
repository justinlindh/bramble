/* Host stub for task.h, scoped to test_wdt_nrf(_*): just enough of the
 * FreeRTOS task-identity surface for wdt_nrf.c's task_id_for() to run.
 * TaskHandle_t is opaque here too; the test binary picks concrete values
 * and supplies pcTaskGetName/xTaskGetCurrentTaskHandle bodies that map them
 * to names, standing in for real distinct FreeRTOS tasks. */
#pragma once

typedef void* TaskHandle_t;

const char* pcTaskGetName(TaskHandle_t task);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
