/*
 * Spike-only no-op peripheral drivers for the IDF linux target: button,
 * battery, WiFi manager, and the device-only half of the OTA component.
 * Each implements an existing firmware header function-for-function; the
 * spike proved these symbols are required to link app_main -> mesh_task on
 * the simulator (their real drivers need esp_driver_gpio / esp_adc /
 * esp_wifi / app_update, none of which exist on the linux target).
 * Replaced by button_virt / battery_virt / emu_link drivers in later tasks.
 */
#include <string.h>

#include "battery.h"
#include "button.h"
#include "ota.h"
#include "ota_rollback.h"
#include "wifi_manager.h"

/* ── button.h ───────────────────────────────────────────────────────── */

void button_init(void) {}

ui_button_t button_poll(uint32_t now_ms) {
    (void)now_ms;
    return BTN_NONE;
}

/* ── battery.h ──────────────────────────────────────────────────────── */

void battery_init(void) {}

uint32_t battery_read_mv(void) { return 4000; }

uint8_t battery_mv_to_pct(uint32_t mv) {
    if (mv >= 4200)
        return 100;
    if (mv <= 3300)
        return 0;
    return (uint8_t)((mv - 3300) * 100 / (4200 - 3300));
}

uint8_t battery_read_pct(void) { return battery_mv_to_pct(battery_read_mv()); }

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

/* ── ota.h / ota_rollback.h (no esp_https_ota / app_update on host).
 *    The pure ota_url.c / ota_version.c / ota_origin.c still compile. ── */

const char* ota_get_last_error(void) { return NULL; }

int ota_ble_start(void) { return -1; }

int ota_wifi_start(const char* url, bool allow_downgrade) {
    (void)url;
    (void)allow_downgrade;
    return -1;
}

const char* ota_get_running_partition(void) { return "sim"; }

const char* ota_get_app_version(void) { return "sim"; }

void ota_rollback_note_boot(void) {}

int ota_rollback_gate(const char* new_version, bool allow_downgrade) {
    (void)new_version;
    (void)allow_downgrade;
    return -1;
}

bool ota_rollback_get_floor(char* out, size_t out_len) {
    (void)out;
    (void)out_len;
    return false;
}
