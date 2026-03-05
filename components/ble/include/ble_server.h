#ifndef BRAMBLE_BLE_SERVER_H
#define BRAMBLE_BLE_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Initialize BLE GATT server with NUS (Nordic UART Service).
 * Registers with rpc_dispatcher for notifications.
 * Returns 0 on success.
 */
int ble_server_init(void);

/**
 * Start BLE advertising. Call after ble_server_init().
 * Returns 0 on success.
 */
int ble_server_start(void);

/**
 * Stop BLE advertising and disconnect any clients.
 */
void ble_server_stop(void);

/**
 * Check if a BLE client is connected.
 */
bool ble_server_connected(void);

/**
 * Send a JSON-RPC notification to the connected BLE client.
 * Data is chunked to fit BLE MTU.
 */
int ble_server_notify(const char* json, size_t len);

#endif
