// Dev-only bench provisioning for the nRF52840 target, the compile-time
// analogue of the emulator's EMU_NETWORK_KEY env seeding (emu_provision.c):
// until P2 lands an RPC transport there is no runtime provisioning path, so
// the bench key enters via a local -DBRAMBLE_NRF_DEV_NETKEY=<64 hex>
// configure argument. The KEY VALUE is never committed anywhere; without the
// define this function compiles to a no-op and the node stays INERT.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "network_key.h"

static const char* TAG = "nrf_provision";

#ifdef BRAMBLE_NRF_DEV_NETKEY

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int nrf_seed_network_key_from_build(void) {
    const char* hex = BRAMBLE_NRF_DEV_NETKEY;
    if (strlen(hex) != 64) {
        ESP_LOGE(TAG, "BRAMBLE_NRF_DEV_NETKEY must be 64 hex chars, node stays INERT");
        return -1;
    }
    uint8_t key[32];
    for (size_t i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            ESP_LOGE(TAG, "BRAMBLE_NRF_DEV_NETKEY has non-hex chars, node stays INERT");
            return -1;
        }
        key[i] = (uint8_t)((hi << 4) | lo);
    }
    network_key_set_provisioned(key);
    memset(key, 0, sizeof(key));
    ESP_LOGW(TAG, "network key seeded from BUILD-TIME dev define (bench only)");
    return 0;
}

#else

int nrf_seed_network_key_from_build(void) { return -1; }

#endif
