#ifndef BRAMBLE_CRYPTO_ENTROPY_H
#define BRAMBLE_CRYPTO_ENTROPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Entropy gate guarding crypto_random() against running before a hardware
 * entropy source exists. On ESP32-S3, esp_random() is only cryptographically
 * secure once either an RF subsystem (Wi-Fi/BT) is running OR the bootloader
 * SAR-ADC entropy source has been enabled via bootloader_random_enable().
 * Identity/key generation runs before RF bring-up (main.c), so main() enables
 * the bootloader entropy source and opens this gate around key generation, then
 * closes it in the entropy-free window and re-opens it once an RF source is up.
 */
void crypto_entropy_set_ready(bool ready);
bool crypto_entropy_is_ready(void);

/*
 * Fail-closed RNG core, shared by crypto_random() (device) and host tests.
 * If the gate is shut: zero the whole buffer and return -1, so even a caller
 * that discards the return value can never leak predictable bytes into a key.
 * If open: fill buf 4 bytes at a time from source() and return 0. The entropy
 * source is injected (esp_random on device) so the fail-closed path is
 * host-testable without pulling in the ESP-IDF RNG.
 */
int crypto_entropy_fill(uint8_t* buf, size_t len, uint32_t (*source)(void));

#endif /* BRAMBLE_CRYPTO_ENTROPY_H */
