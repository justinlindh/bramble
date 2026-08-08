/*
 * Stub for builds without a BLE stack (CONFIG_BT_NIMBLE_ENABLED unset, and
 * the POSIX/Linux simulator). Every entry point reports "unsupported": a
 * stub that pretended to succeed made a device switched to BLE mode log
 * "BLE server started" while advertising nothing, with WiFi off, which is
 * indistinguishable from a working node until someone tries to connect.
 * conn_mode_resolve_boot uses ble_server_supported() to fall back to WiFi
 * at boot, and the settings UIs use it to reject a switch to BLE mode.
 */
#include "ble_server.h"

bool ble_server_supported(void) { return false; }
int ble_server_init(void) { return -1; }
int ble_server_start(void) { return -1; }
void ble_server_stop(void) {}
bool ble_server_connected(void) { return false; }
int ble_server_notify(const char* json, size_t len) {
    (void)json;
    (void)len;
    return -1;
}

void ble_server_set_passkey_display_cb(ble_passkey_display_cb_t cb) { (void)cb; }
bool ble_server_has_passkey_display(void) { return false; }
int ble_server_wipe_bonds(void) { return 0; }
void ble_server_pairing_config_changed(void) {}
