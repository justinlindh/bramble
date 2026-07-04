#ifndef BRAMBLE_SECURE_NVS_H
#define BRAMBLE_SECURE_NVS_H

#include <stdbool.h>

/*
 * Pure decision for how main() should initialize NVS (SEC-H4). Separated from
 * the ESP-IDF calls so the branch logic, including the plaintext-to-encrypted
 * migration case, is host-testable.
 *
 * keys_cfg_ok and secure_init_ok are deliberately separate signals, NOT
 * folded into one: keys_cfg_ok reflects the keys-layer only (reading or, on
 * first boot, generating the nvs_keys partition's XTS keys), while
 * secure_init_ok reflects whether nvs_flash_secure_init could actually
 * decrypt the main NVS partition with an already-valid key. Only a valid
 * key that still fails to decrypt is a genuine plaintext-to-encrypted
 * migration; a keys-layer failure (missing/corrupt keys partition, a failed
 * key read or generate) must fail closed instead, since erasing on that
 * signal would wipe the device on any transient or persistent keys-layer
 * fault, including re-wiping on every subsequent boot if the fault persists.
 */
typedef enum {
    NVS_INIT_PLAIN,        /* encryption off: normal nvs_flash_init */
    NVS_INIT_SECURE,       /* keys valid and secure init ok: proceed */
    NVS_INIT_SECURE_ERASE, /* keys valid but secure init failed (real migration): erase + retry */
    NVS_INIT_FAIL /* encryption build but keys partition missing OR keys-layer read/generate failed
                   */
} nvs_init_action_t;

nvs_init_action_t nvs_init_plan(bool encryption_enabled, bool keys_partition_found,
                                bool keys_cfg_ok, bool secure_init_ok);

#endif /* BRAMBLE_SECURE_NVS_H */
