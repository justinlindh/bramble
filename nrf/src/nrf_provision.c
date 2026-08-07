// Dev-only bench provisioning for the nRF52840 target, the compile-time
// analogue of the emulator's EMU_NETWORK_KEY env seeding (emu_provision.c).
// The REAL provisioning path is bramble.setNetworkKey over the encrypted BLE
// link (the P2 exit gate runs it end to end from an erased device); this
// define is bench convenience only, so a freshly flashed dev kit joins the
// mesh without a pairing round-trip. It enters via a local
// -DBRAMBLE_NRF_DEV_NETKEY=<64 hex> configure argument. The KEY VALUE is
// never committed anywhere; without the define this function compiles to a
// no-op and the node stays INERT until provisioned over BLE.
#include "nrf_provision.h"

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

#ifdef BRAMBLE_NRF_DEV_AUTH_TOKEN

#include "nvs.h"
#include "nvs_keys.h"
#include <string.h>

/* Consoleless boards (T1000-E) cannot surface the auto-minted RPC token: the
 * mint is logged once over a UART that does not exist there. Until the real
 * first-pairing flow for consoleless devices lands, a locally generated
 * token can be seeded at configure time exactly like the dev network key.
 * Seeds only when NO token exists, so a device that already minted or was
 * given one keeps it. The KEY VALUE is never committed anywhere.
 *
 * Returns 0 when a token is now in place (seeded here, or already stored),
 * BRAMBLE_TOKEN_SEED_SKIPPED when this build carries no dev token at all,
 * and -1 only on a real failure. The skipped case needs its own value
 * because it is the NORMAL result for every build that does not pass
 * -DBRAMBLE_NRF_DEV_AUTH_TOKEN: folding it into -1 put a failing return
 * code in the boot trace of every healthy production boot, which cost
 * someone reading a trace the time to rule it out. */
int nrf_seed_auth_token_from_build(void) {
    if (strlen(BRAMBLE_NRF_DEV_AUTH_TOKEN) < 16) {
        ESP_LOGE(TAG, "BRAMBLE_NRF_DEV_AUTH_TOKEN too short (16 char minimum), not seeded");
        return -1;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    char existing[64];
    size_t len = sizeof(existing);
    if (nvs_get_str(h, NVS_KEY_AUTH_TOKEN, existing, &len) == ESP_OK && existing[0] != '\0') {
        nvs_close(h);
        return 0; /* a token already exists; keep it */
    }
    int rc = nvs_set_str(h, NVS_KEY_AUTH_TOKEN, BRAMBLE_NRF_DEV_AUTH_TOKEN) == ESP_OK &&
                     nvs_commit(h) == ESP_OK
                 ? 0
                 : -1;
    nvs_close(h);
    if (rc == 0) {
        ESP_LOGW(TAG, "RPC auth token seeded from BUILD-TIME dev define (bench only)");
    }
    return rc;
}

#else

/* No build-time token compiled in, which is the normal production case and
 * not a failure: ws_server_load_token() mints one immediately after. */
int nrf_seed_auth_token_from_build(void) { return BRAMBLE_TOKEN_SEED_SKIPPED; }

#endif
