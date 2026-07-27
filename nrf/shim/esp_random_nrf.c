// Hardware RNG backend for esp_random on the nRF52840. Uses the RNG
// peripheral via the HAL directly (polling): simple, no interrupt handler,
// and the throughput (one byte per ~120us with bias correction) is fine for
// key generation and jitter draws.
#include "esp_random.h"

#include <hal/nrf_rng.h>

#include "crypto_entropy.h"

void esp_random_nrf_init(void) {
    nrf_rng_error_correction_enable(NRF_RNG);
    nrf_rng_event_clear(NRF_RNG, NRF_RNG_EVENT_VALRDY);
    nrf_rng_task_trigger(NRF_RNG, NRF_RNG_TASK_START);
    crypto_entropy_set_ready(true);
}

static uint8_t rng_byte(void) {
    while (!nrf_rng_event_check(NRF_RNG, NRF_RNG_EVENT_VALRDY)) {
    }
    uint8_t value = nrf_rng_random_value_get(NRF_RNG);
    nrf_rng_event_clear(NRF_RNG, NRF_RNG_EVENT_VALRDY);
    return value;
}

uint32_t esp_random(void) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = (v << 8) | rng_byte();
    }
    return v;
}

void esp_fill_random(void* buf, size_t len) {
    uint8_t* p = buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = rng_byte();
    }
}
