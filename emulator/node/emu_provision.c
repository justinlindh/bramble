/*
 * Emulator provisioning seed for the IDF linux target.
 *
 * The real firmware boots UNPROVISIONED and INERT: with no network key it
 * neither beacons nor carries traffic (mesh_task.c mesh_load_network_key). On a
 * device an operator provisions the key over RPC; the emu-link protocol has no
 * RPC/provisioning message, so a headless scenario has no in-band way to key up
 * a fleet. This seeds the shared network key from the EMU_NETWORK_KEY
 * environment variable (64 hex chars = 32 bytes) at boot, so a gosim scenario
 * can hand every firmware node the SAME key and get a real, meshing fleet that
 * exchanges beacons and channel messages.
 *
 * EMU_NETWORK_KEY unset/blank: no-op, the node stays inert exactly as before
 * (a standalone ./bramble-node run and the unprovisioned-INERT scenarios are
 * unchanged). network_key_set_provisioned also persists the key to NVS, so it
 * survives a supervisor restart the same as an operator-provisioned key.
 *
 * Host-only: built only by emulator/node (null_drivers) on the linux target and
 * called from mesh_load_network_key under a CONFIG_IDF_TARGET_LINUX guard, so a
 * real esp32s3 build never compiles or links it.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "network_key.h"

static const char *TAG = "emu_provision";

/* Parses one hex nibble, or -1 if c is not a hex digit. */
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Seeds the shared network key from EMU_NETWORK_KEY. Returns 0 if a key was
 * provisioned, -1 otherwise (unset, or malformed: not exactly 64 hex chars). */
int emu_node_seed_network_key_from_env(void) {
    const char *hex = getenv("EMU_NETWORK_KEY");
    if (!hex || !*hex)
        return -1;

    if (strlen(hex) != 64) {
        ESP_LOGW(TAG, "EMU_NETWORK_KEY must be 64 hex chars (32 bytes), got %zu; ignoring",
                 strlen(hex));
        return -1;
    }

    uint8_t key[32];
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            ESP_LOGW(TAG, "EMU_NETWORK_KEY has a non-hex character; ignoring");
            return -1;
        }
        key[i] = (uint8_t)((hi << 4) | lo);
    }

    network_key_set_provisioned(key);
    ESP_LOGI(TAG, "network key seeded from EMU_NETWORK_KEY (emulator provisioning)");
    return 0;
}
