// Bramble nRF52840 target entry point: console, RNG, crypto self-check,
// blink/heartbeat tasks, then the boot task hands the node to the mesh
// (app_init_stack -> mesh_task_start).
#include <FreeRTOS.h>
#include <task.h>

#include <hal/nrf_gpio.h>
#include <nrfx.h>

#include <string.h>

#include "app_init.h"
#include "console.h"
#include "crypto.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "bramble_board.h"

void bramble_mbedtls_platform_init(void);

#ifndef BRAMBLE_GIT_DESCRIBE
#define BRAMBLE_GIT_DESCRIBE "unknown"
#endif

static const char* TAG = "main";

uint32_t bramble_log_timestamp_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void bramble_assert_failed(const char* file, int line) {
    /* Name the task: an assert deep in the kernel or a vendored stack is
     * near-useless without knowing who called it. */
    const char* who = "pre-scheduler";
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        who = pcTaskGetName(NULL);
    }
    ESP_LOGE(TAG, "assert failed in task '%s': %s:%d (sched=%d icsr=0x%08lx)", who ? who : "?",
             file, line, (int)xTaskGetSchedulerState(), (unsigned long)SCB->ICSR);
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

    // Deterministic keypair: the point is executing the Ed25519 provider on
    // this silicon each boot, not benchmarking keygen (the real, entropy-gated
    // generation runs on first boot via identity_generate_and_save).
    static const uint8_t seed[32] = {0xB7};
    uint8_t pub[BRAMBLE_ED25519_PUBKEY_SIZE];
    uint8_t priv[BRAMBLE_ED25519_SECKEY_SIZE];
    configASSERT(crypto_ed25519_keypair_from_seed(seed, pub, priv) == 0);

    uint8_t sig[BRAMBLE_ED25519_SIG_SIZE];
    int64_t t0 = esp_timer_get_time();
    configASSERT(crypto_ed25519_sign(priv, (const uint8_t*)"bench", 5, sig) == 0);
    configASSERT(crypto_ed25519_verify(pub, (const uint8_t*)"bench", 5, sig));
    sig[0] ^= 1;
    configASSERT(!crypto_ed25519_verify(pub, (const uint8_t*)"bench", 5, sig));
    int64_t t1 = esp_timer_get_time();

    ESP_LOGI(TAG, "crypto self-check ok: sign+2xverify %lu ms", (unsigned long)((t1 - t0) / 1000));
    crypto_secure_wipe(priv, sizeof(priv));
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
