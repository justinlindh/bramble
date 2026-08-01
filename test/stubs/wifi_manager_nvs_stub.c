/*
 * Strong, controllable override of wifi_manager_nvs_set_creds's weak default
 * in test/stubs/wifi_manager.h.
 *
 * A separate translation unit on purpose: both callers this stub exists for
 * (main/rpc_methods.c's bramble.setWifiConfig, and main/ws_server.c's
 * captive-portal config POST handler, included directly into test_ws_server.c
 * per its "no seams added" design) reach wifi_manager_nvs_set_creds through
 * wifi_manager.h's weak default. A strong definition in the SAME translation
 * unit as either of those would be a redefinition; a strong definition here,
 * linked in as an extra object, is what the linker resolves in preference to
 * the weak one, with zero changes to the files under test.
 *
 * Defaults to success (rc=0) so every existing test that never touches
 * g_wifi_set_creds_rc keeps exercising the success path unchanged; a test
 * that wants to drive an NVS write failure sets it to -1 before dispatching.
 */
#include <string.h>

int g_wifi_set_creds_rc = 0;
char g_wifi_set_creds_ssid[33] = "";
char g_wifi_set_creds_password[65] = "";

int wifi_manager_nvs_set_creds(const char* ssid, const char* password) {
    if (ssid) {
        strncpy(g_wifi_set_creds_ssid, ssid, sizeof(g_wifi_set_creds_ssid) - 1);
        g_wifi_set_creds_ssid[sizeof(g_wifi_set_creds_ssid) - 1] = '\0';
    }
    if (password) {
        strncpy(g_wifi_set_creds_password, password, sizeof(g_wifi_set_creds_password) - 1);
        g_wifi_set_creds_password[sizeof(g_wifi_set_creds_password) - 1] = '\0';
    }
    return g_wifi_set_creds_rc;
}
