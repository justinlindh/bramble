// Access to the mounted settings filesystem for the other nRF-target
// consumers (the message store). The NVS shim owns the mount so there is
// exactly one lfs_t and one partition on this device.
#pragma once

#include "lfs.h"

// Returns the mounted filesystem, or NULL when nvs_flash_init has not
// succeeded (callers must treat NULL as "no persistence").
lfs_t* nvs_lfs_handle(void);

// The filesystem lock. littlefs is not thread-safe and the nvs_* API only
// protects its own calls, so every direct lfs_* access to the handle above
// must sit between these. Recursive use is NOT supported; take it at the
// outermost call.
void nvs_lfs_lock(void);
void nvs_lfs_unlock(void);
