// Dev-only bench provisioning for the nRF52840 target, the compile-time
// analogue of the emulator's EMU_NETWORK_KEY env seeding (emu_provision.c).
// The REAL provisioning path is bramble.setNetworkKey over the encrypted BLE
// link (the P2 exit gate runs it end to end from an erased device); this
// define is bench convenience only, so a freshly flashed dev kit joins the
// mesh without a pairing round-trip. It enters via a local
// -DBRAMBLE_NRF_DEV_NETKEY=<64 hex> configure argument. The KEY VALUE is
// never committed anywhere; without the define this function compiles to a
// no-op and the node stays INERT until provisioned over BLE.
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
