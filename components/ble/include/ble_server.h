#ifndef BRAMBLE_BLE_SERVER_H
#define BRAMBLE_BLE_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Whether this build carries a real BLE stack. False on stub builds
 * (CONFIG_BT_NIMBLE_ENABLED unset, POSIX/Linux simulator), where init and
 * start fail: callers gate mode selection and boot fallback on this.
 */
bool ble_server_supported(void);

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

/**
 * Passkey display hook. When registered (display boards), pairing runs in
 * passkey-display mode: a fresh random 6-digit code per pairing attempt is
 * passed to the callback with show=true, and show=false clears it when the
 * pairing attempt ends (success, failure, or disconnect). The callback runs
 * on the NimBLE host task: it must marshal to its own UI context and must
 * not block. Register before ble_server_init().
 */
typedef void (*ble_passkey_display_cb_t)(uint32_t passkey, bool show);
void ble_server_set_passkey_display_cb(ble_passkey_display_cb_t cb);

/** True when a passkey display callback is registered (board has a display
 * path); bramble.setBlePasskey is rejected on such boards. */
bool ble_server_has_passkey_display(void);

/**
 * Static passkey was set, changed, or cleared: re-apply the SM policy for
 * subsequent pairing attempts and wipe all stored bonds (bonds created
 * under the previous policy stay trusted otherwise, which would make the
 * new passkey theater for already-bonded peers).
 */
void ble_server_pairing_config_changed(void);

#endif
