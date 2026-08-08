#ifndef BRAMBLE_BLE_PAIRING_STORE_H
#define BRAMBLE_BLE_PAIRING_STORE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Persistence for the operator-set static SMP passkey (displayless boards).
 * NVS_NS_BRAMBLE / NVS_KEY_BLE_PASSKEY, stored as u32 0..999999. The value
 * is write-only at the RPC surface (bramble.setBlePasskey); nothing ever
 * reports it back out.
 */

int ble_pairing_store_set(uint32_t passkey);
int ble_pairing_store_clear(void);
bool ble_pairing_store_get(uint32_t* out);
bool ble_pairing_store_is_set(void);

#endif /* BRAMBLE_BLE_PAIRING_STORE_H */
