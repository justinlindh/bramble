#include "ble_server.h"

int ble_server_init(void) { return 0; }
int ble_server_start(void) { return 0; }
void ble_server_stop(void) {}
bool ble_server_connected(void) { return false; }
int ble_server_notify(const char* json, size_t len) {
    (void)json;
    (void)len;
    return 0;
}
