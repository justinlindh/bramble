/*
 * Host-test entropy source for nRF-backend crypto builds: crypto_esp.c calls
 * esp_fill_random (mbedtls blinding RNG) and crypto_random goes through the
 * crypto_entropy gate, which is fail-closed at boot. The constructor opens
 * the gate for tests, mirroring what esp_random_nrf_init does on the device
 * once the hardware RNG runs.
 */
#include <stddef.h>
#include <stdint.h>
#include <sys/random.h>

#include "crypto_entropy.h"

uint32_t esp_random(void) {
    uint32_t v = 0;
    (void)!getrandom(&v, sizeof(v), 0);
    return v;
}

void esp_fill_random(void* buf, size_t len) { (void)!getrandom(buf, len, 0); }

__attribute__((constructor)) static void open_entropy_gate(void) { crypto_entropy_set_ready(true); }
