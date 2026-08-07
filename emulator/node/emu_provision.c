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
#include "location.h"
#include "network_key.h"
#include "nvs.h"
#include "nvs_keys.h"

static const char* TAG = "emu_provision";

/* Declared here rather than by including mesh_task.h: this file builds as part
 * of the null_drivers component, which does not carry main/ on its include
 * path, and main.c declares this file's own entry points the same way. */
int mesh_add_channel(const char* name, const uint8_t* psk, size_t psk_len);

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
    const char* hex = getenv("EMU_NETWORK_KEY");
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

/*
 * Seeds a channel location-share target from the environment.
 *
 * Location sharing is configured over RPC (bramble.setLocationConfig), and
 * emu-link carries no RPC, so a headless scenario has no in-band way to turn
 * sharing on. This writes the same location namespace RPC writes, through the
 * same rule codec and key builder the send path reads back, so a scenario
 * exercises the real policy tick rather than a shortcut around it.
 *
 * EMU_LOCATION_CHANNEL_PSK is the pre-shared key of the location channel.
 * Every node that sets it JOINS that channel, which a receiver needs in order
 * to decrypt at all. EMU_LOCATION_SHARE additionally makes this node share to
 * it; a receiver sets the PSK alone and stays silent, so a scenario can tell a
 * real transmit apart from a node decoding its own broadcast. Both unset is a
 * no-op and the node shares nothing, which is the privacy-first default every
 * other scenario keeps. EMU_LOCATION_INTERVAL_S and EMU_LOCATION_TIER are
 * optional overrides.
 *
 * The channel is CREATED here from that PSK rather than named by index,
 * because that is the real provisioning order: the index is an output of
 * adding a channel, not an input a caller may assume, and two nodes need not
 * land on the same slot. Nodes agree because they derive the same key from the
 * same PSK. The public channel cannot carry location (its key is well known),
 * so a scenario has to bring its own keyed channel.
 *
 * Returns 0 if a share target was written, -1 otherwise.
 */
int emu_node_seed_location_share_from_env(void) {
    const char* psk = getenv("EMU_LOCATION_CHANNEL_PSK");
    if (!psk || !*psk)
        return -1;

    int channel_index = mesh_add_channel("emu-loc", (const uint8_t*)psk, strlen(psk));
    if (channel_index < 0) {
        ESP_LOGW(TAG, "could not add the location channel");
        return -1;
    }
    ESP_LOGI(TAG, "location channel joined at index %d (emulator provisioning)", channel_index);

    const char* share = getenv("EMU_LOCATION_SHARE");
    if (!share || !*share || share[0] == '0') {
        /* Channel member, not a sharer. */
        return -1;
    }

    char key[LOCATION_TARGET_KEY_SIZE];
    if (!location_channel_key(key, sizeof(key), channel_index)) {
        ESP_LOGW(TAG, "channel index %d is outside the channel-target key space; ignoring",
                 channel_index);
        return -1;
    }
    if (!location_channel_target_is_permitted(channel_index)) {
        ESP_LOGW(TAG, "channel index %d cannot carry location; not seeding a target",
                 channel_index);
        return -1;
    }

    const char* interval_str = getenv("EMU_LOCATION_INTERVAL_S");
    uint16_t interval_s = LOCATION_MIN_INTERVAL_S;
    if (interval_str && *interval_str) {
        int parsed = atoi(interval_str);
        if (parsed > 0)
            interval_s = location_policy_clamp_interval_s((uint16_t)parsed);
    }

    const char* tier_str = getenv("EMU_LOCATION_TIER");
    uint8_t tier =
        (tier_str && *tier_str) ? location_tier_from_string(tier_str) : LOCATION_TIER_FULL;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "location namespace unavailable; not seeding a share target");
        return -1;
    }

    location_rule_t rule = {.enabled = true, .tier = tier, .interval_s = interval_s};
    char value[48];
    location_rule_format(value, sizeof(value), &rule);

    esp_err_t err = nvs_set_u8(nvs, "enabled", 1);
    if (err == ESP_OK)
        err = nvs_set_u16(nvs, "interval_s", interval_s);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "def_tier", location_tier_to_string(tier));
    if (err == ESP_OK)
        err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "location share seed failed: %d", err);
        return -1;
    }

    ESP_LOGI(TAG, "location share seeded from env: %s=%s (emulator provisioning)", key, value);
    return 0;
}
