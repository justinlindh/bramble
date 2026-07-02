#include "secure_nvs.h"

nvs_init_action_t nvs_init_plan(bool encryption_enabled, bool keys_partition_found,
                                bool prior_secure_init_ok) {
    if (!encryption_enabled) {
        return NVS_INIT_PLAIN;
    }
    if (!keys_partition_found) {
        return NVS_INIT_FAIL;
    }
    return prior_secure_init_ok ? NVS_INIT_SECURE : NVS_INIT_SECURE_ERASE;
}
