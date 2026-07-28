/*
 * Platform stubs for the RPC surface on the nRF52840 target.
 *
 * main/rpc_methods.c registers one table for the whole fleet, and a few of
 * its handlers need hardware this target does not have: Wi-Fi (so no OTA
 * download, no origin allowlist, no Wi-Fi status) and a display backlight.
 * Every stub here answers honestly rather than pretending: callers get a
 * clearly failed result, not a fake success. The handlers themselves stay
 * registered so the webapp sees the same method list on every fleet and can
 * tell "not supported here" from "unknown method".
 *
 * OTA on this target will arrive over BLE or the bootloader's own DFU path
 * (P3 work); when it does, these download-side stubs are what it replaces.
 */
#include <stddef.h>
#include <string.h>

#include <stdbool.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "ota.h"
#include "wifi_manager.h"

static const char* TAG = "rpc_stubs";

/* ── Wi-Fi ───────────────────────────────────────────────────────────── */

void wifi_manager_get_status(wifi_status_t* status) {
    if (status != NULL) {
        memset(status, 0, sizeof(*status)); /* this chip has no Wi-Fi radio */
    }
}

/* ── OTA download half (needs Wi-Fi + HTTPS) ─────────────────────────── */
/* Note: ota_resolve_artifact, ota_progress_*, ota_origin_* and ota_version_*
 * are portable and compiled from components/ota; only the download and
 * partition halves are stubbed here. */

int ota_wifi_start(const char* url, bool allow_downgrade) {
    (void)url;
    (void)allow_downgrade;
    ESP_LOGW(TAG, "OTA over Wi-Fi is not available on this hardware");
    return -1;
}

const char* ota_get_last_error(void) { return "OTA is not supported on this hardware"; }

const char* ota_get_app_version(void) {
    const esp_app_desc_t* desc = esp_app_get_description();
    return desc != NULL ? desc->version : "unknown";
}

const char* ota_get_running_partition(void) { return "app"; }

bool ota_rollback_get_floor(char* out, size_t out_len) {
    (void)out;
    (void)out_len;
    return false; /* no anti-rollback floor without OTA on this target */
}

/* ── Display ─────────────────────────────────────────────────────────── */

void display_set_backlight(int percent) { (void)percent; /* no display on this board */ }

/* ── WebSocket origin allowlist (an HTTP-server concept) ─────────────── */

static const char s_no_origins[] = "";

const char* ws_server_get_extra_origins(void) { return s_no_origins; }

void ws_server_load_origins(void) {}
