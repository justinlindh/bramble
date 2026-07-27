// Bramble nRF52840 target entry point. P0 bring-up: FreeRTOS scheduler with
// blink and heartbeat tasks over the bench-verified UART console.
#include <FreeRTOS.h>
#include <task.h>

#include <hal/nrf_gpio.h>

#include "console.h"
#include "esp_log.h"
#include "wio_wm1110_devkit.h"

#ifndef BRAMBLE_GIT_DESCRIBE
#define BRAMBLE_GIT_DESCRIBE "unknown"
#endif

static const char* TAG = "main";

uint32_t bramble_log_timestamp_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void bramble_assert_failed(const char* file, int line) {
    ESP_LOGE(TAG, "assert failed: %s:%d", file, line);
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char* name) {
    (void)task;
    ESP_LOGE(TAG, "stack overflow in task %s", name ? name : "?");
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

void vApplicationMallocFailedHook(void) {
    ESP_LOGE(TAG, "FreeRTOS heap exhausted");
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

static void task_blink(void* arg) {
    (void)arg;
    nrf_gpio_cfg_output(BOARD_PIN_LED1);
    for (;;) {
        nrf_gpio_pin_toggle(BOARD_PIN_LED1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void task_heartbeat(void* arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "heartbeat: uptime %lu ms, free heap %u bytes",
                 (unsigned long)bramble_log_timestamp_ms(), (unsigned)xPortGetFreeHeapSize());
    }
}

int main(void) {
    console_init();
    ESP_LOGI(TAG, "Bramble nRF52840 P0 %s booted", BRAMBLE_GIT_DESCRIBE);

    BaseType_t ok = xTaskCreate(task_blink, "blink", 256, NULL, 1, NULL);
    configASSERT(ok == pdPASS);
    ok = xTaskCreate(task_heartbeat, "heartbeat", 512, NULL, 2, NULL);
    configASSERT(ok == pdPASS);

    ESP_LOGI(TAG, "starting scheduler");
    vTaskStartScheduler();
    // Never reached.
    for (;;) {
    }
}
