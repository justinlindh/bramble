/*
 * Randomness for the nRF52840 target.
 *
 * The hardware RNG peripheral cannot be read directly once BLE is running:
 * NimBLE's link layer claims RNG for its own use, and its interrupt handler
 * consumes each VALRDY event before anyone else sees it. A caller polling
 * VALRDY therefore spins forever (observed exactly once: the mesh task hung
 * on its first beacon the moment BLE came up, with the radio task still
 * happily servicing interrupts around it).
 *
 * So the peripheral is read exactly once, at boot and before BLE starts, to
 * seed a CTR-DRBG; every later draw comes from that. This also removes the
 * per-byte busy-wait from the hot path: a DRBG draw is a few AES blocks
 * rather than ~120us of polling per byte, which matters because dummy
 * traffic and key exchange pull hundreds of bytes at a time.
 */
#include "esp_random.h"

#include <limits.h>
#include <string.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#include <hal/nrf_rng.h>

#include <mbedtls/ctr_drbg.h>

#include "crypto_entropy.h"
#include "esp_log.h"

static const char* TAG = "esp_random";

static mbedtls_ctr_drbg_context s_drbg;
static bool s_ready;
static bool s_hw_owned;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

/* Reads the hardware RNG with bias correction. Only valid while this code
 * still owns the peripheral, which is only true during seeding. */
static uint8_t hw_rng_byte(void) {
    while (!nrf_rng_event_check(NRF_RNG, NRF_RNG_EVENT_VALRDY)) {
    }
    uint8_t value = nrf_rng_random_value_get(NRF_RNG);
    nrf_rng_event_clear(NRF_RNG, NRF_RNG_EVENT_VALRDY);
    return value;
}

static int seed_from_hw(void* ctx, unsigned char* out, size_t len) {
    (void)ctx;
    if (!s_hw_owned) {
        /* Refuse rather than poll: after BLE bring-up this would spin
         * forever waiting for a VALRDY the link layer has already taken. */
        return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = hw_rng_byte();
    }
    return 0;
}

void esp_random_nrf_init(void) {
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);

    s_hw_owned = true;
    nrf_rng_error_correction_enable(NRF_RNG);
    nrf_rng_event_clear(NRF_RNG, NRF_RNG_EVENT_VALRDY);
    nrf_rng_task_trigger(NRF_RNG, NRF_RNG_TASK_START);

    mbedtls_ctr_drbg_init(&s_drbg);
    /* A full CTR-DRBG seed (entropy plus nonce, 48 bytes) drawn from the
     * hardware while this code still owns the peripheral. */
    int rc = mbedtls_ctr_drbg_seed(&s_drbg, seed_from_hw, NULL, NULL, 0);

    /* Hand the peripheral over: from here on it belongs to the BLE link
     * layer, and seed_from_hw refuses rather than polling it. */
    nrf_rng_task_trigger(NRF_RNG, NRF_RNG_TASK_STOP);
    s_hw_owned = false;

    /* Never auto-reseed. The only hardware entropy source is gone once BLE
     * owns it, so a reseed could only fail; a 384-bit seed covers far more
     * draws than this device will make in its lifetime. */
    mbedtls_ctr_drbg_set_reseed_interval(&s_drbg, INT_MAX);

    if (rc != 0) {
        /* Fail closed: crypto_random stays shut and the node refuses to mint
         * keys rather than minting predictable ones. */
        ESP_LOGE(TAG, "DRBG seeding failed (%d); randomness unavailable", rc);
        return;
    }
    s_ready = true;
    crypto_entropy_set_ready(true);
    ESP_LOGI(TAG, "DRBG seeded from the hardware RNG");
}

void esp_fill_random(void* buf, size_t len) {
    if (!s_ready) {
        /* Never hand back predictable bytes; the entropy gate above keeps
         * callers from using them, and zeroing makes misuse obvious. */
        memset(buf, 0, len);
        return;
    }
    bool locked = xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
    if (locked) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    int rc = mbedtls_ctr_drbg_random(&s_drbg, buf, len);
    if (locked) {
        xSemaphoreGive(s_lock);
    }
    if (rc != 0) {
        memset(buf, 0, len);
    }
}

uint32_t esp_random(void) {
    uint32_t v = 0;
    esp_fill_random(&v, sizeof(v));
    return v;
}
