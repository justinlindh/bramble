// Bramble nRF52840 target entry point. P0 bring-up: FreeRTOS scheduler with
// blink and heartbeat tasks over the bench-verified UART console.
#include <FreeRTOS.h>
#include <task.h>

#include <hal/nrf_gpio.h>

#include <string.h>

#include "app_init.h"
#include "console.h"
#include "crypto.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "wio_wm1110_devkit.h"

void bramble_mbedtls_platform_init(void);

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

// Boot-time crypto self-check: proves the minimal mbedtls config plus the
// Monocypher Ed25519 provider actually execute on this silicon (the host
// suites prove correctness against standards vectors; this proves the target
// build). Logs one line; failure halts boot loudly.
static void crypto_self_check(void) {
    static const uint8_t sha_abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    uint8_t hash[32];
    configASSERT(crypto_sha256((const uint8_t*)"abc", 3, hash) == 0);
    configASSERT(memcmp(hash, sha_abc, sizeof(hash)) == 0);

    bramble_identity_t id;
    int64_t t0 = esp_timer_get_time();
    configASSERT(crypto_generate_identity(&id) == 0);
    int64_t t1 = esp_timer_get_time();

    uint8_t sig[BRAMBLE_ED25519_SIG_SIZE];
    configASSERT(crypto_ed25519_sign(id.ed25519_private_key, (const uint8_t*)"bench", 5, sig) == 0);
    configASSERT(crypto_ed25519_verify(id.ed25519_public_key, (const uint8_t*)"bench", 5, sig));
    sig[0] ^= 1;
    configASSERT(!crypto_ed25519_verify(id.ed25519_public_key, (const uint8_t*)"bench", 5, sig));
    int64_t t2 = esp_timer_get_time();

    ESP_LOGI(TAG, "crypto self-check ok: addr %08lx, keygen %lu ms, sign+2xverify %lu ms",
             (unsigned long)id.address, (unsigned long)((t1 - t0) / 1000),
             (unsigned long)((t2 - t1) / 1000));
    crypto_secure_wipe(&id, sizeof(id));
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
        // newlib-nano printf has no %lld; keep 32-bit formats on this target.
        ESP_LOGI(TAG, "heartbeat: uptime %lu ms, timer %lu us, rng %08lx, free heap %u bytes",
                 (unsigned long)bramble_log_timestamp_ms(), (unsigned long)esp_timer_get_time(),
                 (unsigned long)esp_random(), (unsigned)xPortGetFreeHeapSize());
    }
}

size_t heap_free_probe(void) { return xPortGetFreeHeapSize(); }

static void task_boot(void* arg) {
    (void)arg;
    crypto_self_check();
    app_init_stack();
    vTaskDelete(NULL);
}

int main(void) {
    console_init();
    esp_random_nrf_init();
    bramble_mbedtls_platform_init();
    ESP_LOGI(TAG, "Bramble nRF52840 P0 %s booted", BRAMBLE_GIT_DESCRIBE);

    // Crypto runs in a task (2KB stack): mbedtls ECP wants more stack than
    // the pre-scheduler main stack guarantees once P1 shrinks it.
    BaseType_t ok = xTaskCreate(task_boot, "boot", 2048, NULL, 3, NULL);
    configASSERT(ok == pdPASS);
    ok = xTaskCreate(task_blink, "blink", 256, NULL, 1, NULL);
    configASSERT(ok == pdPASS);
    ok = xTaskCreate(task_heartbeat, "heartbeat", 512, NULL, 2, NULL);
    configASSERT(ok == pdPASS);

    ESP_LOGI(TAG, "starting scheduler");
    vTaskStartScheduler();
    // Never reached.
    for (;;) {
    }
}
