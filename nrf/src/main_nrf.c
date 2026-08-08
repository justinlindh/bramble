// Bramble nRF52840 target entry point: console, RNG, crypto self-check,
// blink/heartbeat tasks, then the boot task hands the node to the mesh
// (app_init_stack -> mesh_task_start).
#include <FreeRTOS.h>
#include <task.h>

#include <hal/nrf_gpio.h>
#include <nrfx.h>

#include <string.h>

#include "app_init.h"
#include "boot_trace.h"
#include "bramble_wdt.h"
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
    /* Halting here would leave a consoleless board indistinguishable from a
     * dead one; stamp the trace and return to the bootloader instead, where
     * the host can read what happened. */
    taskDISABLE_INTERRUPTS();
    boot_trace_fail(BT_FAIL_ASSERT, (uint32_t)line);
}

void HardFault_Handler(void) {
    /* The startup file's default handler is an infinite loop. Recover the
     * stacked PC so the trace names the faulting instruction. */
    uint32_t* frame;
    __asm volatile("tst lr, #4\n"
                   "ite eq\n"
                   "mrseq %0, msp\n"
                   "mrsne %0, psp\n"
                   : "=r"(frame));
    boot_trace_fail(BT_FAIL_HARDFAULT, frame[6]);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char* name) {
    (void)task;
    ESP_LOGE(TAG, "stack overflow in task %s", name ? name : "?");
    taskDISABLE_INTERRUPTS();
    boot_trace_fail(BT_FAIL_STACK_OVF, 0);
}

void vApplicationMallocFailedHook(void) {
    ESP_LOGE(TAG, "FreeRTOS heap exhausted");
    taskDISABLE_INTERRUPTS();
    boot_trace_fail(BT_FAIL_MALLOC, 0);
}

/* Without this hook the idle task busy-spins: with configUSE_TICKLESS_IDLE 0
 * the vendored idle loop (prvIdleTask in tasks.c) reduces to
 * prvCheckTasksWaitingTermination() plus a conditional taskYIELD(), and the
 * port's only sleep instruction (the WFI in vPortSuppressTicksAndSleep,
 * port.c) is compiled out entirely under `#if configUSE_TICKLESS_IDLE == 1`.
 * So every idle tick previously ran the core flat out at 64MHz for no work.
 *
 * WFE, not WFI: the vendored tickless path wraps its WFI in a cpsid/cpsie
 * critical section specifically to close the race between "check for
 * pending work" and "sleep" (an interrupt landing in that gap would
 * otherwise need the critical section to guarantee WFI sees it pending).
 * We call this hook with interrupts enabled and no critical section, so we
 * need the same guarantee some other way: the ARM event register does it
 * for free. Every exception, including every enabled interrupt, sets the
 * event register on the way out; WFE consumes and clears it, so an event
 * pended anytime since the last WFE falls straight through instead of
 * blocking, and no wakeup can be lost to the timing of when we happen to
 * call this hook.
 *
 * Safe next to NimBLE's link-layer task: the LL's radio and timer ISRs run
 * at hardware IRQ priority regardless of what the CPU is doing when they
 * fire, and any enabled interrupt wakes WFE by definition, so sleeping here
 * cannot delay or drop a radio event. */
void vApplicationIdleHook(void) { __WFE(); }

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
    boot_trace_mark(BT_CRYPTO_CHECK, 0);
    crypto_self_check();
    boot_trace_mark(BT_CRYPTO_OK, 0);
    app_init_stack();
    vTaskDelete(NULL);
}

#ifdef BRAMBLE_BOOT_DFU_RECOVERY
/* Watchdog-of-last-resort for consoleless boards: if advertising is not up
 * two minutes after boot, stamp the last stage reached and return to the
 * bootloader so the trace page is host-readable. High priority so a
 * busy-spinning boot task cannot starve it; it sleeps its whole life.
 *
 * This covers a HANG and only a hang: it needs 120 seconds of scheduler time
 * to fire, so a board that resets faster than that never reaches it. The
 * other half of the recovery story is the consecutive-failed-boot count in
 * boot_trace_init(), which catches exactly the case this task cannot. */
static void task_sentinel(void* arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(120000));
    if (!boot_trace_adv_ok()) {
        boot_trace_fail(BT_FAIL_SENTINEL, boot_trace_last());
    }
    vTaskDelete(NULL);
}
#endif

int main(void) {
    /* The dev kit image links at 0x0 and is flashed over SWD, so the reset
     * value of VTOR (0) already points at our vector table. Behind the
     * T1000-E's UF2 bootloader the app lives at 0x27000 and arrives here by
     * a JUMP, not a reset: VTOR still points at Nordic's MBR, and the
     * bootloader's NVIC state (USB et al) is still live. The first scheduler
     * exception would vector through the MBR/bootloader table and the app
     * dies before any task runs. Claim the vector table and quiesce the
     * NVIC before anything can fire. */
    extern uint32_t __isr_vector[];
    __disable_irq();
    for (unsigned i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    SCB->VTOR = (uint32_t)__isr_vector;
    __DSB();
    __ISB();
    __enable_irq();

    boot_trace_init();
    boot_trace_mark(BT_MAIN_ENTRY, SCB->VTOR);

    console_init();
    esp_random_nrf_init();
    bramble_mbedtls_platform_init();
    ESP_LOGI(TAG, "Bramble nRF52840 P0 %s booted", BRAMBLE_GIT_DESCRIBE);

    /* Initializes the WDT driver only; does not start the countdown (see
     * bramble_wdt.h). Must run before any task exists: every subscriber's
     * esp_task_wdt_add() requires the driver already initialized, and this
     * is the one place that can guarantee it runs first. The countdown
     * itself is armed later, from app_init_stack() after boot reaches
     * steady state; see that call site for why. */
    bramble_wdt_init();

    // Crypto runs in a task (2KB stack): mbedtls ECP wants more stack than
    // the pre-scheduler main stack guarantees once P1 shrinks it.
    BaseType_t ok = xTaskCreate(task_boot, "boot", 2048, NULL, 3, NULL);
    configASSERT(ok == pdPASS);
    ok = xTaskCreate(task_blink, "blink", 256, NULL, 1, NULL);
    configASSERT(ok == pdPASS);
    ok = xTaskCreate(task_heartbeat, "heartbeat", 512, NULL, 2, NULL);
    configASSERT(ok == pdPASS);
#ifdef BRAMBLE_BOOT_DFU_RECOVERY
    /* Same priority as the timer task: above every worker, below the BLE
     * link layer. */
    ok = xTaskCreate(task_sentinel, "sentinel", 256, NULL, configMAX_PRIORITIES - 2, NULL);
    configASSERT(ok == pdPASS);
#endif

    ESP_LOGI(TAG, "starting scheduler");
    vTaskStartScheduler();
    // Never reached.
    for (;;) {
    }
}
