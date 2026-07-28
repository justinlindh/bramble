// esp_random shim for the nRF52840 target, fed by the hardware RNG with bias
// correction (nrf/shim/esp_random_nrf.c).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Blocking hardware RNG init; call once before the scheduler starts. Opens
// the crypto entropy gate (crypto_entropy_set_ready) once the RNG runs.
void esp_random_nrf_init(void);

uint32_t esp_random(void);
void esp_fill_random(void* buf, size_t len);

#ifdef __cplusplus
}
#endif
