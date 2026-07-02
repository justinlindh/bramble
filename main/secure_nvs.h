#ifndef BRAMBLE_SECURE_NVS_H
#define BRAMBLE_SECURE_NVS_H

#include <stdbool.h>

/*
 * Pure decision for how main() should initialize NVS (SEC-H4). Separated from
 * the ESP-IDF calls so the branch logic, including the plaintext-to-encrypted
 * migration case, is host-testable.
 */
typedef enum {
    NVS_INIT_PLAIN,        /* encryption off: normal nvs_flash_init */
    NVS_INIT_SECURE,       /* keys partition present: nvs_flash_secure_init */
    NVS_INIT_SECURE_ERASE, /* secure init failed (format change/migration): erase + retry */
    NVS_INIT_FAIL          /* encryption build but keys partition missing */
} nvs_init_action_t;

nvs_init_action_t nvs_init_plan(bool encryption_enabled, bool keys_partition_found,
                                bool prior_secure_init_ok);

#endif /* BRAMBLE_SECURE_NVS_H */
