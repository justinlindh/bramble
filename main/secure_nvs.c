#include "secure_nvs.h"

nvs_init_action_t nvs_init_plan(bool encryption_enabled, bool keys_partition_found,
                                bool keys_cfg_ok, bool secure_init_ok) {
    if (!encryption_enabled) {
        return NVS_INIT_PLAIN;
    }
    if (!keys_partition_found) {
        return NVS_INIT_FAIL;
    }
    if (!keys_cfg_ok) {
        /* Fail closed: any keys-layer read/generate failure (corrupt keys
         * partition, flash error) must abort, never erase the main
         * partition. Matches the ESP-IDF reference behavior. */
        return NVS_INIT_FAIL;
    }
    return secure_init_ok ? NVS_INIT_SECURE : NVS_INIT_SECURE_ERASE;
}
