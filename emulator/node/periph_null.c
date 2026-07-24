/*
 * No-op peripheral drivers for the IDF linux target: WiFi manager and the
 * device-only half of the OTA component. Each implements an existing firmware
 * header function-for-function; the spike proved these symbols are required to
 * link app_main -> mesh_task on the simulator (their real drivers need
 * esp_wifi / app_update, neither of which exists on the linux target). These
 * remain no-ops (Task 9+ territory).
 *
 * The button and battery halves that used to live here are gone: the real
 * virtual drivers button_virt.c / battery_virt.c now own those symbols on the
 * linux target (and gps_virt.c owns gps). Defining them here too would clash at
 * link time.
 */
#include <string.h>

#include "ota.h"
#include "ota_rollback.h"
#include "wifi_manager.h"

/* ── wifi_manager.h (no esp_wifi on the simulator: always "no wifi") ── */

int wifi_manager_init(uint32_t node_addr) {
    (void)node_addr;
    return -1;
}

void wifi_manager_get_status(wifi_status_t* status) {
    memset(status, 0, sizeof(*status));
    status->mode = BRAMBLE_WIFI_OFF;
}

const char* wifi_manager_get_ip(void) { return ""; }

int wifi_manager_nvs_get_creds(char* ssid, size_t ssid_len, char* password, size_t pass_len) {
    (void)ssid;
    (void)ssid_len;
    (void)password;
    (void)pass_len;
    return -1;
}

int wifi_manager_nvs_set_creds(const char* ssid, const char* password) {
    (void)ssid;
    (void)password;
    return -1;
}

int wifi_manager_nvs_clear_creds(void) { return -1; }

/* The emulator never brings up an AP, so it holds no secret and yields no
 * password. wifi_ap_password_derive itself is real on the linux target. */
void wifi_manager_set_ap_secret(const uint8_t* secret, size_t secret_len) {
    (void)secret;
    (void)secret_len;
}

int wifi_manager_get_ap_password(char* out, size_t out_len) {
    if (out && out_len > 0)
        out[0] = '\0';
    return -1;
}

/* ── ota.h / ota_rollback.h (no esp_https_ota / app_update on host).
 *    The pure ota_url.c / ota_version.c / ota_origin.c still compile. ── */

const char* ota_get_last_error(void) { return NULL; }

int ota_wifi_start(const char* url, bool allow_downgrade) {
    (void)url;
    (void)allow_downgrade;
    return -1;
}

const char* ota_get_running_partition(void) { return "sim"; }

const char* ota_get_app_version(void) { return "sim"; }

void ota_rollback_note_boot(void) {}

int ota_rollback_gate(const char* new_version, uint32_t candidate_secure_version,
                      bool allow_downgrade) {
    (void)new_version;
    (void)candidate_secure_version;
    (void)allow_downgrade;
    return -1;
}

bool ota_rollback_get_floor(char* out, size_t out_len) {
    (void)out;
    (void)out_len;
    return false;
}
