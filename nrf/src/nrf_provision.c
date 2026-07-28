// Dev-only bench provisioning for the nRF52840 target, the compile-time
// analogue of the emulator's EMU_NETWORK_KEY env seeding (emu_provision.c):
// until P2 lands an RPC transport there is no runtime provisioning path, so
// the bench key enters via a local -DBRAMBLE_NRF_DEV_NETKEY=<64 hex>
// configure argument. The KEY VALUE is never committed anywhere; without the
// define this function compiles to a no-op and the node stays INERT.
#include "esp_log.h"
#include "network_key.h"

static const char* TAG = "nrf_provision";

#ifdef BRAMBLE_NRF_DEV_NETKEY

int nrf_seed_network_key_from_build(void) {
    if (network_key_set_from_hex(BRAMBLE_NRF_DEV_NETKEY) != 0) {
        ESP_LOGE(TAG, "BRAMBLE_NRF_DEV_NETKEY malformed (need 64 hex chars), node stays INERT");
        return -1;
    }
    ESP_LOGW(TAG, "network key seeded from BUILD-TIME dev define (bench only)");
    return 0;
}

#else

int nrf_seed_network_key_from_build(void) { return -1; }

#endif
